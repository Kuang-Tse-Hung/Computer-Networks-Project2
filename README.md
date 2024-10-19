# Computer-Networks-Project2
Reliable File Transfer Protoco 


Run recvfile on CLEAR with: `./recvfile -p 18015`

Run Sendfile on LOOK with: `./sendfile -r 128.42.124.178:18015 -f subdir/test_1mb.bin`

### Bugs

1. If sendfile aborted and you start sendfile again, recvfile cannot recv anything in the second time and gets stuck. (which means if sendfile failed and you want to try again, you have to restart both sendfile and recvfile) i dont think this is reasonable
2. Window cannot slide: when I tried to transmit 1mb file, only 1 start packet and 9 data packets (1024 bytes) were sent. (For 1mb file, we will need 1k data packets with size of 1024 bytes! )

