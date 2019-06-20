# tfunnel
tfunnel is a simple VPN implemented on top of [TPROXY](https://www.kernel.org/doc/Documentation/networking/tproxy.txt).

tfunnel works by intercepting TCP connection and UDP packets on the local, *client* instance, forwards them to a remote, *proxy* instance. The *proxy* instance recreates the TCP connections to the original remote destination, and sends the UDP packets as well. The connection between the *client* and the *proxy* can be any bidirectional channel, such an ssh connection.

tfunnel is a replacement for [sshuttle --method=tproxy](https://github.com/sshuttle/sshuttle), which has never worked very well for me.

## Features
* does not require privileged access on the proxy host
* supports IPV6, TCP and UDP
* fast
* connection reset handling for both TCP and UDP (ICMP port unreachable packets are sent)
* load balancing (just launch multiple instances!)

The *client* instance requires either root privileges, or the following capabilities:
```
cap_net_admin=ep cap_net_raw=ep cap_net_bind_service=ep
```

# Routing configuration
tfunnel does not try to setup the firewall like sshuttle does. You have to setup source based routing on your own, in order to have packets sent to tfunnel.

All packets coming from the tfunnel *client* instance have a fwmark=3. This can be exploited to make the iptables ruleset easier.

## Example: tfunnel over SSH in an IPv4 network
Here is the setup to tunnel all your local and forwarded IPv4 traffic through a tfunnel *client* instance, running on port 12300, that forwards all traffic to a *proxy* instance running on host:port `$SSH_HOST:$SSH_PORT`, over ssh.

First of all make sure that tfunnel is available in `$PATH` in the remote host. You start and link the *client* and *proxy* with either the bash coproc feature,
```
coproc tfunnel { stdbuf -i0 -o0 tfunnel -p 12300; }
stdbuf -i0 -o0 ssh $SSH_HOST stdbuf -i0 -o0 tfunnel >&"${tfunnel[1]}" <&"${tfunnel[0]}"
```
or with the socat `EXEC:` target:
```
socat EXEC:'tfunnel -p 12300' EXEC:'ssh $SSH_HOST stdbuf -i0 -o0 tfunnel'
```

Now packets of interest must be intercepted by the *client* instance. We use here source based routing with packet marks. All packets of interest are marked with fwmark=1, and they are looped back to the *client* instance running locally:
```
ip rule add fwmark 1 lookup 100
ip route add local default dev lo table 100
```

The following iptables setup contains the logic for marking the packets:
* `tfunnel-mark-proxied`: mark packets that should be redirected to the tfunnel *client* instance with fwmark=1: all packets except DNS, UDP broadcast/multicast and packets directed towards the proxy endpoint `$SSH_HOST:$SSH_PORT`
* `tfunnel-output`: intercept local outgoing traffic, ignore packets with fwmark=3 (outgoing from tfunnel), traverse `tfunnel-mark-proxied`
* `tfunnel-prerouting`: intercept incoming traffic, ignore packets with fwmark=3 (outgoing from tfunnel), traverse `tfunnel-mark-proxied`, and if it marked as proxied, use TPROXY to effectively redirect to tfunnel
```
iptables-restore <<EOF
    *mangle
    :PREROUTING ACCEPT [0:0]
    :INPUT ACCEPT [0:0]
    :FORWARD ACCEPT [0:0]
    :OUTPUT ACCEPT [0:0]
    :POSTROUTING ACCEPT [0:0]
    :tfunnel-mark-proxied - [0:0]
    :tfunnel-output - [0:0]
    :tfunnel-prerouting - [0:0]
    -A PREROUTING -j tfunnel-prerouting
    -A OUTPUT -j tfunnel-output
    -A tfunnel-mark-proxied -d $SSH_HOST/32 -p tcp -m tcp --dport $SSH_PORT -j RETURN
    -A tfunnel-mark-proxied -s $SSH_HOST/32 -p tcp -m tcp --sport $SSH_PORT -j RETURN
    -A tfunnel-mark-proxied -p udp -m udp --dport 53 -j RETURN
    -A tfunnel-mark-proxied -p udp -m udp --sport 53 -j RETURN
    -A tfunnel-mark-proxied -m addrtype --dst-type LOCAL -j RETURN
    -A tfunnel-mark-proxied -p udp -m addrtype --dst-type MULTICAST -j RETURN
    -A tfunnel-mark-proxied -p udp -m addrtype --dst-type BROADCAST -j RETURN
    -A tfunnel-mark-proxied -p tcp -m tcp -j MARK --set-xmark 0x1/0x1
    -A tfunnel-mark-proxied -p udp -m udp -j MARK --set-xmark 0x1/0x1
    -A tfunnel-output -m mark --mark 0x2/0x2 -j RETURN
    -A tfunnel-output -j tfunnel-mark-proxied
    -A tfunnel-prerouting -m mark --mark 0x2/0x2 -j RETURN
    -A tfunnel-prerouting -j tfunnel-mark-proxied
    -A tfunnel-prerouting -m mark ! --mark 0x1/0x1 -j RETURN
    -A tfunnel-prerouting -p tcp -m tcp -j TPROXY --on-port 12300
    -A tfunnel-prerouting -p udp -m udp -j TPROXY --on-port 12300
    COMMIT
EOF
```

### IPv6
Support for IPv6 has not been tested thoroughly yet. The above example should work on an IPv6 network with minimal changes. Just replace `ip rule`, `ip route` and `iptables-restore` with `ip -6 rule`, `ip -6 route` and `ip6tables-restore`, and take note that `-m addrtype --dst-type BROADCAST` is not valid in ip6tables. A single instance of tfunnel will suffice capturing IPv4 and IPv6 traffic.

# Command line options
```
tfunnel options:
  -v [ --verbose ]                enable verbose output
  -p arg (=0)                     start in client mode and listen on this port
  --udp_timeout arg (=30)         timeout for unanswered UDP connections
  --udp_timeout_stream arg (=120) timeout for answered UDP connections
  --help                          print help
```
The udp options control the timeouts for the UDP sockets that are created to forward the traffic. They default to the values of netfilter (`/proc/sys/net/netfilter/nf_conntrack_udp_timeout` and `/proc/sys/net/netfilter/nf_conntrack_udp_timeout_stream`).

# Building
Requirements are Boost.Asio, Boost.Program\_options, Boost.Coroutine . Build system is CMake:
```
git clone https://github.com/pisto/tfunnel.git
mkdir tfunnel/build
cd tfunnel/build
cmake -DCMAKE_BUIL_TYPE=Release ..

make -j
sudo make install     #set correct capabilities with setcap
```

## Proxy-only build
On platforms other than Linux, only the *proxy* part of tfunnel is built, and the executable name is `tfunnel-proxy-only`. This build can be forced also by passing the option `-DPROXY_ONLY=ON` during the cmake invocation. tfunnel should compile on all platforms supported by [Boost.Coroutine](https://www.boost.org/doc/libs/1_70_0/libs/context/doc/html/context/architectures.html).
