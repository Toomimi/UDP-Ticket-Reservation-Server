# UDP Ticket Reservation Server

One threaded implementation of IPv4, UDP server which handles ticket reservation for events. Only server is mine, client and examples were provided as a part of task for 2021/2022 Computer Networking course at University of Warsaw. 


### How to run?
- compile: ```g++ -Wall -Wextra -Wno-implicit-fallthrough -std=c++17 -O2 -o server```
- run: ```./server -f <reservations_description> -p <port> -t <timeout>```
  - -f: (Mandatory) Description of events. Each event is described in 2 lines (max 80 chars). First is any String description, the second is integer value between 0 and 65535 which is available tickets count. (look example).
  - -p: (Optional, Default: 2022) Port on which server listens
  - -t: (Optional, Default: 5) Server timeout time, value between 0 and 86400
  
  
### Messages between server and client
**TODO!**
    
