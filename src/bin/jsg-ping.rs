use std::{io::Write, mem::MaybeUninit, os::fd::AsRawFd, vec};

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

fn test_rtt(comms: &EthernetComms, data_size: usize) -> Result<std::time::Duration, String> {
    let ping_string = "ping".to_string() + &" ".repeat(data_size - 8) + "ping";
    let pong_string = "pong".to_string() + &" ".repeat(data_size - 8) + "pong";
    let mut recv_buf = vec![0_u8; data_size];

    let start_time = std::time::Instant::now();

    comms
        .send_frame(ping_string.as_bytes())
        .map_err(perror_fmt("Can't send ping message"))?;
    comms
        .recv_frame(&mut recv_buf)
        .map_err(perror_fmt("Can't receive pong message"))?;

    let end_time = start_time.elapsed();

    if recv_buf != pong_string.as_bytes() {
        return Err("The received message is not equal to the pong message expected".to_string());
    }
    Ok(end_time)
}

fn main() -> Result<(), String> {
    let args: Vec<String> = std::env::args().collect();
    let num_tests = args
        .get(1)
        .ok_or("The first command line argument must be the number of tests")
        .and_then(|s| {
            str::parse(s).map_err(|_| "The number of tests must be a vaild positive integer")
        })?;
    let data_size: usize = args
        .get(2)
        .ok_or("The second command line argument must be the size of a ping payload")
        .and_then(|s| {
            str::parse(s).map_err(|_| "The payload size must be a vaild positive integer")
        })?;
    let ifname = &args
        .get(3)
        .ok_or("The interface name isn't specified in the first command line argument")?;
    let comms = EthernetComms::new(0xDEAD, ifname, [0xE2, 0x18, 0xE1, 0x2C, 0xF9, 0x79])?;

    let mut rtt = std::time::Duration::ZERO;
    let mut rtt_times_ms = Vec::<f32>::with_capacity(num_tests);

    println!(
        "Starting tests with {} packets of {} bytes...",
        num_tests, data_size
    );
    for _ in 0..num_tests {
        let sample_time = test_rtt(&comms, data_size)?;
        rtt_times_ms.push(sample_time.as_secs_f32() * 1000.0);
        rtt += sample_time;
    }

    rtt_times_ms.sort_unstable_by(|l, r| l.partial_cmp(r).unwrap());

    println!(
        "RTT min/avg/med/max: {:.3}/{:.3}/{:.3}/{:.3} ms",
        rtt_times_ms.first().as_ref().unwrap(),
        rtt.as_secs_f32() * 1000.0 / num_tests as f32,
        rtt_times_ms[rtt_times_ms.len() / 2],
        rtt_times_ms.last().as_ref().unwrap(),
    );

    Ok(())
}
