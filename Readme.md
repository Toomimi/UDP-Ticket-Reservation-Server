# UDP Ticket Reservation Server

One threaded implementation of IPv4, UDP server which handles ticket reservation for events.


### How to run?
- compile:
- ./server -f <reservations_description> -p <port> -t <timeout>
  - -f: Description of events. Each event is described in 2 lines (max 80 chars). First is any String description, the second is integer value between 0 and 65535 which is available tickets count.
  
  
### Messages between server and client
**TODO!**
    
