## Loadbalancer Instructions
```
./loadbalancer [balancer port] [server ports] -R [requests] -N [connections]
```
The loadbalancer takes as arguments 1 mandatory port number for the balancer itself, as well as one or more port numbers for the servers. Note that the loadbalancer port must always appear before the server ports. The other arguments are optional. Requests refers to the number of requests that should occur before the balancer performs a healthcheck. Connections refers to the number of simultaneous connections the balancer should service. For optimal performance, the number of connections should be equal to or greater than the total number of connections available across all servers (This may break down for a large number of servers as all threads are competing for CPU time).  
  
There are no known bugs.