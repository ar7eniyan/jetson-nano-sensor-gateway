use std::os::fd::AsRawFd;

use nix::{
    libc::{sockaddr_ll, AF_PACKET},
    net::if_::if_nametoindex,
    sys::socket::{
        bind, recvfrom, sendto, sockaddr, socket, AddressFamily, LinkAddr, MsgFlags, SockFlag,
        SockProtocol, SockType, SockaddrLike,
    },
};

const IFNAME: &str = "enp4s0";

fn perror_fmt(message: impl AsRef<str>) -> impl Fn(nix::errno::Errno) -> String {
    move |errno| message.as_ref().to_string() + ": " + errno.desc()
}

#[derive(Debug)]
struct EthernetComms {
    sockfd: std::os::fd::OwnedFd,
    peer_address: LinkAddr,
}

impl EthernetComms {
    fn new(ethertype: u16, ifname: &str, peer_mac: [u8; 6]) -> Result<Self, String> {
        let ifindex = if_nametoindex(IFNAME).map_err(perror_fmt(
            "Can't get the index of the interface".to_owned() + ifname,
        ))?;
        println!("Using interface {ifname} (index {ifindex})");

        let sockfd = socket(
            AddressFamily::Packet,
            SockType::Datagram,
            SockFlag::empty(),
            SockProtocol::EthAll, // Do I need to call htons()?
        )
        .map_err(perror_fmt("Can't create link layer socket"))?;

        let mut peer_address = sockaddr_ll {
            sll_family: AF_PACKET.try_into().unwrap(), // Should always be AF_PACKET
            sll_protocol: ethertype.to_be(),           // For bind()
            sll_ifindex: ifindex.try_into().unwrap(),  // For bind()
            sll_hatype: 0,
            sll_pkttype: 0,
            sll_halen: 6,     // For further sendto()
            sll_addr: [0; 8], // For futher sendto()
        };
        peer_address.sll_addr[..6].clone_from_slice(&peer_mac);
        let peer_address = unsafe {
            LinkAddr::from_raw(&peer_address as *const sockaddr_ll as *const sockaddr, None)
                .unwrap()
        };

        bind(sockfd.as_raw_fd(), &peer_address)
            .map_err(perror_fmt("Can't bind to the interface and the protocol"))?;

        Ok(Self {
            sockfd,
            peer_address,
        })
    }

    fn send_frame(&self, frame: &[u8]) -> nix::Result<usize> {
        // Should I check the frame size to be within IEEE802.3 limits (46-1500 bytes)?
        // Or is it done inside the implementation?
        sendto(
            self.sockfd.as_raw_fd(),
            frame,
            &self.peer_address,
            MsgFlags::empty(),
        )
    }

    fn recv_frame(&self, buf: &mut [u8]) -> nix::Result<usize> {
        loop {
            let (read_len, recv_addr) = recvfrom::<LinkAddr>(self.sockfd.as_raw_fd(), buf)
                .map(|(len, addr)| (len, addr.unwrap()))?;

            debug_assert_eq!(recv_addr.hatype(), nix::libc::ARPHRD_ETHER);
            debug_assert_eq!(recv_addr.halen(), 6);
            debug_assert_eq!(
                u16::from_be(recv_addr.protocol()),
                self.peer_address.protocol()
            );
            // 0 below is PACKET_HOST in Linux API, TODO: use the definition
            // from libc crate when it get added there.
            if recv_addr.pkttype() == 0 && recv_addr.addr() == self.peer_address.addr() {
                return Ok(read_len);
            }
        }
    }
}

fn main() -> Result<(), String> {
    let comms = EthernetComms::new(0xDEAD, "enp4s0", [0; 6])?;
    dbg!(comms);

    Ok(())
}
