<div align="center">

### Overview

Chat provides a straightforward tcp chat communication cli in local host.

### Dependencies

Linux operating system

Standard TCP/IP libraries (`arpa/inet.h`)

### Installation

Clone the repository and install the executable using the following commands:

<pre>
git clone https://github.com/x3ric/chat
cd chat
sudo make install
</pre>

### Usage

Once installed,  you can run following command:

<pre>
chat # start chat server or connect locally if already running
</pre>

### Network Configuration Options

Expand Chat's capabilities beyond the local host through various methods:

#### Port Forwarding

> Note: Not secure; may expose shell access. Use locally only.

Configure port forwarding for the server's port. The server provides the exact command to connect via port forwarding.

#### Third-Party Server Proxy

Services like :

[**Haguichi**](https://github.com/ztefn/haguichi): Manage Hamachi VPN connections on Linux. 

[**Ngrok**](https://ngrok.com/download): Create secure tunnels to localhost for temporary access and testing.

[**ZeroTier**](https://www.zerotier.com/download/): Easily manage virtual networks and establish secure connections.

<br>
<a href="https://archlinux.org">
  <img alt="Arch Linux" src="https://img.shields.io/badge/Arch_Linux-1793D1?style=for-the-badge&logo=arch-linux&logoColor=D9E0EE&color=000000&labelColor=97A4E2"/>
</a>

</div>
