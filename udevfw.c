/*
 * Copyright (c) 2016 Mackenzie Straight. See LICENSE for license details.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <asm/types.h>
#include <errno.h>
#include <fcntl.h>
#include <libudev.h>
#include <linux/netlink.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <pthread.h>

#include "MurmurHash2.h"

#define UDEV_MONITOR_UDEV 2
#define UDEV_MONITOR_MAGIC 0xFEEDCAFE

typedef struct MessageHeader
{
    char prefix[8];
    unsigned int magic;
    unsigned int headerSize;
    unsigned int propertiesOffset;
    unsigned int propertiesLength;
    unsigned int filterSubsystemHash;
    unsigned int filterDeviceTypeHash;
    unsigned int filterTagBloomLow;
    unsigned int filterTagBloomHigh;
} MessageHeader;

typedef struct QueueEntry
{
    struct QueueEntry *next;
    struct udev_device *dev;
} QueueEntry;

int nsfd;
pthread_cond_t qcond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
struct QueueEntry *qhead;

static unsigned int stringHash(const char *str)
{
    return MurmurHash2(str, strlen(str), 0);
}

static unsigned long long bloomHash(const char *str)
{
    unsigned long long bits = 0;
    unsigned int hash = stringHash(str);

    bits |= 1ULL << (hash & 63);
    bits |= 1ULL << ((hash >> 6) & 63);
    bits |= 1ULL << ((hash >> 12) & 63);
    bits |= 1ULL << ((hash >> 18) & 63);
    return bits;
}

static int sendDeviceMessage(int fd, struct udev_device *dev)
{
    MessageHeader header = {
        "libudev", htonl(UDEV_MONITOR_MAGIC), sizeof(MessageHeader)
    };
    struct sockaddr_nl saddr = { AF_NETLINK, 0, 0, UDEV_MONITOR_UDEV };
    char *buf = 0;
    int buflen = 0, bufpos = 0;
    unsigned long long tagBits = 0;
    struct udev_list_entry *prop, *tag;
    const char *subsys, *devtype;
    struct iovec iov[2] = {
        { &header, sizeof(header) },
    };
    struct msghdr message = {
        (void *)&saddr, sizeof(struct sockaddr_nl), iov, 2
    };

    subsys = udev_device_get_subsystem(dev);
    devtype = udev_device_get_devtype(dev);

    if (subsys) {
        header.filterSubsystemHash = htonl(stringHash(subsys));
    }

    if (devtype) {
        header.filterDeviceTypeHash = htonl(stringHash(devtype));
    }

    udev_list_entry_foreach(tag,
        udev_device_get_tags_list_entry(dev)) {

        tagBits |= bloomHash(udev_list_entry_get_name(tag));
    }

    header.filterTagBloomLow = htonl(tagBits & 0xFFFFFFFF);
    header.filterTagBloomHigh = htonl(tagBits >> 32);

    udev_list_entry_foreach(prop,
        udev_device_get_properties_list_entry(dev)) {

        const char *name = udev_list_entry_get_name(prop);
        const char *val = udev_list_entry_get_value(prop);
        int namelen = strlen(name);
        int vallen = strlen(val);
        int proplen = namelen + vallen + 2;

        buf = realloc(buf, buflen + proplen);
        buflen += proplen;
        memcpy(buf + bufpos, name, namelen);
        bufpos += namelen;
        buf[bufpos] = '=';
        bufpos++;
        memcpy(buf + bufpos, val, vallen);
        bufpos += vallen;
        buf[bufpos] = '\0';
        bufpos++;
    }

    buf = realloc(buf, buflen + 1);
    buflen++;
    buf[buflen - 1] = '\0';
    header.propertiesOffset = sizeof(MessageHeader);
    header.propertiesLength = buflen;
    iov[1].iov_base = buf;
    iov[1].iov_len = buflen;
    return sendmsg(fd, &message, 0);
}

static void *namespaceThreadStart(void *unused)
{
    int sendfd;

    if (setns(nsfd, CLONE_NEWNET) < 0) {
        perror("setns");
        exit(1);
    }

    sendfd = socket(PF_NETLINK,
        SOCK_RAW|SOCK_CLOEXEC|SOCK_NONBLOCK,
        NETLINK_KOBJECT_UEVENT);

    if (sendfd < 0) {
        perror("socket");
        exit(1);
    }

    for (;;) {
        pthread_mutex_lock(&qlock);

        if (!qhead) {
            pthread_cond_wait(&qcond, &qlock);
        }

        while (qhead) {
            QueueEntry *next = qhead->next;
            struct udev_device *dev = qhead->dev;

            free(qhead);
            qhead = next;
            sendDeviceMessage(sendfd, dev);
            udev_device_unref(dev);
        }

        pthread_mutex_unlock(&qlock);
    }

    return NULL;
}

int main(int argc, const char *argv[])
{
    int epollfd, monitorfd;
    pthread_t nsthread;
    struct udev *uctx;
    struct udev_monitor *monitor;
    struct epoll_event ev[8] = {};

    if (argc != 2) {
        fprintf(stderr, "Syntax: %s <netns-path>\n", argv[0]);
        return 1;
    }

    nsfd = open(argv[1], O_RDONLY);

    if (nsfd < 0) {
        perror("open");
        return 1;
    }

    uctx = udev_new();
    monitor = udev_monitor_new_from_netlink(uctx, "udev");

    if (!monitor) {
        fprintf(stderr, "Failed to create monitor\n");
        return 1;
    }

    epollfd = epoll_create1(EPOLL_CLOEXEC);

    if (epollfd < 0) {
        perror("epoll_create1");
        return 1;
    }

    monitorfd = udev_monitor_get_fd(monitor);

    if (udev_monitor_enable_receiving(monitor)) {
        fprintf(stderr, "Failed to enable receiving\n");
    }

    ev[0].events = EPOLLIN;
    ev[0].data.fd = monitorfd;

    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, monitorfd, &ev[0]) < 0) {
        perror("epoll_ctl");
        return 1;
    }

    if (pthread_create(&nsthread, 0, namespaceThreadStart, 0)) {
        perror("pthread_create");
        return 1;
    }

    for (;;) {
        int i, nevents = epoll_wait(epollfd, ev, 8, -1);

        if (nevents < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("epoll_wait");
            return 1;
        }

        for (i = 0; i < nevents; ++i) {
            if (ev[i].events & EPOLLIN) {
                struct udev_device *dev = udev_monitor_receive_device(monitor);

                if (dev) {
                    QueueEntry *entry =
                        (QueueEntry *)malloc(sizeof(QueueEntry));

                    pthread_mutex_lock(&qlock);
                    entry->next = qhead;
                    entry->dev = dev;
                    qhead = entry;
                    pthread_cond_signal(&qcond);
                    pthread_mutex_unlock(&qlock);
                }
            }
        }
    }

    return 0;
}

