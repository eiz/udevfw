# udevfw

Forwards udev events into a given network namespace. You must provide the path
to a file suitable for use with `setns(2)`, e.g.

   ip netns add my-namespace
   udevfw /run/netns/my-namespace

`udevfw` must be run as root, or with `CAP_NET_ADMIN` rights.

## Bugs

This program is somewhat hacky because systemd's `libudev` library does not
export the necessary function to send udev messages. As a result, it relies
upon the specific implementation details of the udev message format.
