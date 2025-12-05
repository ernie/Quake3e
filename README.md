# Quake3e Trinity Server

This branch on this fork of Quake3e is specifically intended for use with [Trinity](https://github.com/ernie/trinity).

This dedicated-server-only build has only minor changes:

1. It allows overriding all Quake 3 master servers in your server config, so you can list only where you want to.
2. It adds additional data in its status reporting for use by the Trinity data collector.

## Building

```sh
make
```

That's it.
