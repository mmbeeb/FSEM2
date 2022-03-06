# FSEM2
File Server Emulator 2

Improved version which uses L3V126 file server ($.FS).

## Advanced networking

By default the server will listen as station 254 on `0.0.0.0:10254`

The station ID can be changed with the option `-s` flag.  eg `-s 100` will set the server to station 100 listening on `0.0.0.0:10100`

This works well for BeebEm but will not work with RiscOS nor with [PiEconetBridge](https://github.com/cr12925/PiEconetBridge) in RiscOS compatibility mode.

With RiscOS the port is always 32768, and the station ID is based on the last octet of the IP address.  So if your IP address is 192.168.1.100 then you would be station 100.

This can be set up with the `-a` option.  You should set the `-s` station ID as well.  eg  `-a 192.168.1.100 -s 100`.  Now it will listen on `192.168.1.100:32768`
