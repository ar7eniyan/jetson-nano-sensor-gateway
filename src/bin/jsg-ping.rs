use std::{mem::MaybeUninit, os::fd::AsRawFd};

use nix::{
    libc::{sockaddr_ll, AF_PACKET, ARPHRD_ETHER},
    sys::socket::{
        bind, recvfrom, sendto, sockaddr, socket, AddressFamily, LinkAddr, MsgFlags, SockFlag,
        SockProtocol, SockType, SockaddrLike,
    },
};

fn perror_fmt(message: impl AsRef<str>) -> impl Fn(nix::errno::Errno) -> String {
    move |errno| message.as_ref().to_string() + ": " + errno.desc()
}

nix::ioctl_readwrite_bad!(
    ioctl_get_if_index,
    nix::libc::SIOCGIFINDEX,
    nix::libc::ifreq
);
nix::ioctl_readwrite_bad!(
    ioctl_get_if_hwaddr,
    nix::libc::SIOCGIFHWADDR,
    nix::libc::ifreq
);

#[derive(Debug)]
struct EthernetComms {
    sockfd: std::os::fd::OwnedFd,
    peer_address: LinkAddr,
}

impl EthernetComms {
    fn new(ethertype: u16, ifname: &str, peer_mac: [u8; 6]) -> Result<Self, String> {
        let sockfd = socket(
            AddressFamily::Packet,
            SockType::Datagram,
            SockFlag::empty(),
            SockProtocol::EthAll,
        )
        .map_err(perror_fmt("Can't create link layer socket"))?;

        if !ifname.is_ascii() {
            return Err("The interface name must be an ASCII string".to_string());
        }
        if ifname.len() >= nix::libc::IFNAMSIZ {
            return Err(format!(
                "Interface name too long: {} characters, less than {} expected",
                ifname.len(),
                nix::libc::IFNAMSIZ
            ));
        }
        // ifreq.ifr_name gets filled with zeros, so we only need to ensure not to override
        // the last NULL byte to have a valid C string there.
        let mut ifreq = unsafe { MaybeUninit::<nix::libc::ifreq>::zeroed().assume_init() };
        for i in 0..ifname.len() {
            ifreq.ifr_name[i] = ifname.as_bytes()[i] as _;
        }

        let mac_addr: [i8; 6] = unsafe {
            ioctl_get_if_hwaddr(sockfd.as_raw_fd(), &mut ifreq).map_err(perror_fmt(format!(
                "Can't get the hardware address of the interface {ifname}"
            )))?;
            if ifreq.ifr_ifru.ifru_hwaddr.sa_family != ARPHRD_ETHER {
                return Err(format!(
                    "The type of the interface \"{ifname}\" is not ARPHRD_ETHERNET"
                ));
            }
            ifreq.ifr_ifru.ifru_hwaddr.sa_data[..6].try_into().unwrap()
        };
        let ifindex = unsafe {
            ioctl_get_if_index(sockfd.as_raw_fd(), &mut ifreq).map_err(perror_fmt(format!(
                "Can't get the index of the interface \"{ifname}\""
            )))?;
            ifreq.ifr_ifru.ifru_ifindex
        };

        println!("Binding to the interface \"{ifname}\"");
        println!("    ifindex {}", ifindex);
        println!(
            "    mac {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
            mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]
        );

        let mut peer_address = sockaddr_ll {
            sll_family: AF_PACKET.try_into().unwrap(), // Should always be AF_PACKET
            sll_protocol: ethertype.to_be(),           // For bind()
            sll_ifindex: ifindex,                      // For bind()
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
            debug_assert_eq!(recv_addr.protocol(), self.peer_address.protocol());
            // 0 below is PACKET_HOST in Linux API, TODO: use the definition
            // from libc crate when it get added there.
            if recv_addr.pkttype() == 0 && recv_addr.addr() == self.peer_address.addr() {
                return Ok(read_len);
            }
        }
    }
}

fn main() -> Result<(), String> {
    let args: Vec<String> = std::env::args().collect();
    let ifname = &args
        .get(1)
        .ok_or("The interface name isn't specified in the first command line argument")?;
    let comms = EthernetComms::new(0xDEAD, ifname, [0xE2, 0x18, 0xE1, 0x2C, 0xF9, 0x79])?;

    let ping_string = "ping".to_string() + &" ".repeat(40) + "ping";
    let pong_string = "pong".to_string() + &" ".repeat(40) + "pong";
    let mut recv_buf = vec![0_u8; pong_string.len()];

    comms
        .send_frame(ping_string.as_bytes())
        .map_err(perror_fmt("Can't send ping message"))?;
    println!("Sent ping");

    comms
        .recv_frame(&mut recv_buf)
        .map_err(perror_fmt("Can't receive pong message"))?;
    if recv_buf != pong_string.as_bytes() {
        return Err("The received message is not equal to the pong message expected".to_string());
    }
    println!("Received pong");

    Ok(())
}
