# tfunnel
Better documentation coming soon, I promise.

tfunnel is a simple VPN implemented with TPROXY. It works by intercepting TCP connections and UDP packets locally and communicates with STDOUT and STDOUT with a remote demuxer, which recreates the connections on the remote machine. The demuxer can be connected with a ssh connection, a SSL connection, anything works.

tfunnel It is a replacement for [sshuttle](https://github.com/sshuttle/sshuttle) with the tproxy method, which has never worked very well for me.

## Features
* does not require privileged access on the proxy box (requires CAP\_NET\_ADMIN on the local box)
* supports IPV6, TCP and UDP
* simple
* fast (?)
* better TCP reset handling compared to sshuttle

TODO
* forward ICMP errors for UDP "connections"

# Running
Better documentation coming soon, I told you.

Run `ftunnel --help` for a synopsis of the arguments. The client is started by selecting a port to listen to with the `-p` argument. The proxy does not take any argument. For example this starts the client locally on port 12345 and runs the proxy on the ssh host "freedom" (the binary must be present in the remote box):
```
socat EXEC:'ftunnel -p 12345' EXEC:'ssh freedom ftunnel'
```
Local iptables and IP routing setup is up to you. Better documentation coming soon, really.

# Building
Requirements are Boost.Asio, Boost.Program\_options, Boost.Coroutine . Build system is CMake:
```
git clone https://github.com/pisto/tfunnel.git
mkdir tfunnel/build
cd tfunnel/build
cmake -DCMAKE_BUIL_TYPE=Release ..

#build executable that can be run both as the client and the proxy
make -j
#or, if you want a stripped down proxy executable
make -j tfunnel-proxy-only
```
The client is likely only compatible with Linux. The stripped down `tfunnel-proxy-only` proxy executable can probably be built and run for all the architectures [that are supported by Boost.Coroutine](https://www.boost.org/doc/libs/1_70_0/libs/context/doc/html/context/architectures.html).
