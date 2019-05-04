# tftp-server
TFTP server implemented in c

## Features
- Uses I/O Multiplexing (`select()` api) to handle concurrent clients
- Uses timeouts and retries to ensure delivery of messages
- (Currently supports only RRQ)
