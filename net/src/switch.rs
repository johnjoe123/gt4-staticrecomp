//! One lobby's virtual Ethernet segment: a learning switch for up to six members, plus the two services a console
//! expects from a home router, answered by the switch itself: DHCP (so "automatic" network settings work) and ARP
//! for the router address. Pure logic, no I/O.

use crate::MAX_PLAYERS;

pub const MIN_FRAME: usize = 14;
pub const MAX_FRAME: usize = 1518;
/// The virtual router: 192.168.47.1. Members get 192.168.47.11 .. 192.168.47.16 by slot.
pub const SUBNET: [u8; 3] = [192, 168, 47];
pub const ROUTER_IP: [u8; 4] = [192, 168, 47, 1];
pub const ROUTER_MAC: [u8; 6] = [0x02, 0x47, 0x54, 0x34, 0x00, 0x01];
const BROADCAST: [u8; 6] = [0xFF; 6];

pub fn slot_ip(slot: usize) -> [u8; 4] {
    [SUBNET[0], SUBNET[1], SUBNET[2], 11 + slot as u8]
}

/// A frame to deliver to one member slot.
pub type Delivery = (usize, Vec<u8>);

#[derive(Default)]
pub struct Switch {
    present: [bool; MAX_PLAYERS],
    /// Learned source addresses: (MAC, slot).
    table: Vec<([u8; 6], usize)>,
}

impl Switch {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn join(&mut self) -> Option<usize> {
        let slot = self.present.iter().position(|p| !p)?;
        self.present[slot] = true;
        Some(slot)
    }

    pub fn leave(&mut self, slot: usize) {
        if slot < MAX_PLAYERS {
            self.present[slot] = false;
        }
        self.table.retain(|(_, s)| *s != slot);
    }

    pub fn count(&self) -> usize {
        self.present.iter().filter(|p| **p).count()
    }

    pub fn is_empty(&self) -> bool {
        self.count() == 0
    }

    fn learn(&mut self, mac: [u8; 6], slot: usize) {
        if mac[0] & 1 != 0 {
            return; // a group address is never a source
        }
        if let Some(entry) = self.table.iter_mut().find(|(m, _)| *m == mac) {
            entry.1 = slot;
        } else if self.table.len() < 64 {
            self.table.push((mac, slot));
        }
    }

    fn others(&self, from: usize) -> impl Iterator<Item = usize> + '_ {
        (0..MAX_PLAYERS).filter(move |s| *s != from && self.present[*s])
    }

    /// Takes one frame from `from` and returns where it goes. Malformed frames are dropped.
    pub fn on_frame(&mut self, from: usize, frame: &[u8]) -> Vec<Delivery> {
        if from >= MAX_PLAYERS || !self.present[from] || frame.len() < MIN_FRAME || frame.len() > MAX_FRAME {
            return Vec::new();
        }
        let dst: [u8; 6] = frame[0..6].try_into().unwrap();
        let src: [u8; 6] = frame[6..12].try_into().unwrap();
        self.learn(src, from);
        if let Some(reply) = dhcp_reply(from, frame).or_else(|| arp_reply(frame)) {
            return vec![(from, reply)];
        }
        if is_dhcp_request(frame) {
            return Vec::new(); // a DHCP message we do not answer never reaches other consoles
        }
        if dst == ROUTER_MAC {
            return Vec::new();
        }
        if dst[0] & 1 == 0 {
            if let Some((_, slot)) = self.table.iter().find(|(m, _)| *m == dst) {
                return if *slot != from && self.present[*slot] { vec![(*slot, frame.to_vec())] } else { Vec::new() };
            }
        }
        self.others(from).map(|s| (s, frame.to_vec())).collect()
    }
}

fn ethertype(frame: &[u8]) -> u16 {
    u16::from_be_bytes([frame[12], frame[13]])
}

/// Offset of the UDP header when the frame is an unfragmented IPv4 UDP datagram to `port`.
fn udp_to_port(frame: &[u8], port: u16) -> Option<usize> {
    if ethertype(frame) != 0x0800 || frame.len() < 14 + 20 + 8 {
        return None;
    }
    let ip = &frame[14..];
    let ihl = usize::from(ip[0] & 0x0F) * 4;
    if ip[0] >> 4 != 4 || ihl < 20 || ip[9] != 17 || ip.len() < ihl + 8 {
        return None;
    }
    if u16::from_be_bytes([ip[6], ip[7]]) & 0x3FFF != 0 {
        return None;
    }
    let udp = 14 + ihl;
    (u16::from_be_bytes([frame[udp + 2], frame[udp + 3]]) == port).then_some(udp)
}

fn is_dhcp_request(frame: &[u8]) -> bool {
    udp_to_port(frame, 67).is_some()
}

fn checksum(data: &[u8]) -> u16 {
    let mut sum = 0u32;
    for pair in data.chunks(2) {
        sum += u32::from(pair[0]) << 8 | u32::from(*pair.get(1).unwrap_or(&0));
    }
    while sum >> 16 != 0 {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    !(sum as u16)
}

/// Answers DHCPDISCOVER with an offer and DHCPREQUEST with an acknowledgement; the address depends only on the slot.
fn dhcp_reply(slot: usize, frame: &[u8]) -> Option<Vec<u8>> {
    let udp = udp_to_port(frame, 67)?;
    let bootp = frame.get(udp + 8..)?;
    if bootp.len() < 240 || bootp[0] != 1 || bootp[236..240] != [99, 130, 83, 99] {
        return None;
    }
    let mut kind = 0u8;
    let mut requested: Option<[u8; 4]> = None;
    let mut options = &bootp[240..];
    while let [code, rest @ ..] = options {
        match *code {
            0 => options = rest,
            255 => break,
            _ => {
                let (&len, rest) = rest.split_first()?;
                let value = rest.get(..usize::from(len))?;
                match (*code, value) {
                    (53, [k]) => kind = *k,
                    (50, [a, b, c, d]) => requested = Some([*a, *b, *c, *d]),
                    _ => {}
                }
                options = &rest[usize::from(len)..];
            }
        }
    }
    let mine = slot_ip(slot);
    let reply_kind = match kind {
        1 => 2u8, // DISCOVER -> OFFER
        3 => {
            // REQUEST -> ACK, or NAK when the console asks for an address that is not its slot's
            let ciaddr: [u8; 4] = bootp[12..16].try_into().unwrap();
            let wanted = requested.unwrap_or(ciaddr);
            if wanted == mine || wanted == [0; 4] { 5 } else { 6 }
        }
        _ => return None,
    };
    let mut b = vec![0u8; 240];
    b[0] = 2; // BOOTREPLY
    b[1] = 1;
    b[2] = 6;
    b[4..8].copy_from_slice(&bootp[4..8]); // xid
    b[10..12].copy_from_slice(&bootp[10..12]); // flags
    if reply_kind != 6 {
        b[16..20].copy_from_slice(&mine); // yiaddr
    }
    b[20..24].copy_from_slice(&ROUTER_IP); // siaddr
    b[28..44].copy_from_slice(&bootp[28..44]); // chaddr
    b[236..240].copy_from_slice(&[99, 130, 83, 99]);
    b.extend_from_slice(&[53, 1, reply_kind, 54, 4]);
    b.extend_from_slice(&ROUTER_IP);
    if reply_kind != 6 {
        b.extend_from_slice(&[51, 4, 0, 1, 0x51, 0x80]); // lease: one day
        b.extend_from_slice(&[1, 4, 255, 255, 255, 0]);
        b.extend_from_slice(&[3, 4]);
        b.extend_from_slice(&ROUTER_IP);
        b.extend_from_slice(&[6, 4]);
        b.extend_from_slice(&ROUTER_IP);
        b.extend_from_slice(&[28, 4, SUBNET[0], SUBNET[1], SUBNET[2], 255]);
    }
    b.push(255);
    while b.len() < 300 {
        b.push(0); // BOOTP minimum
    }
    let udp_len = 8 + b.len();
    let total = 20 + udp_len;
    let mut out = Vec::with_capacity(14 + total);
    out.extend_from_slice(&BROADCAST);
    out.extend_from_slice(&ROUTER_MAC);
    out.extend_from_slice(&[0x08, 0x00]);
    let mut ip = [0u8; 20];
    ip[0] = 0x45;
    ip[2..4].copy_from_slice(&(total as u16).to_be_bytes());
    ip[8] = 64;
    ip[9] = 17;
    ip[12..16].copy_from_slice(&ROUTER_IP);
    ip[16..20].copy_from_slice(&[255; 4]);
    let sum = checksum(&ip);
    ip[10..12].copy_from_slice(&sum.to_be_bytes());
    out.extend_from_slice(&ip);
    out.extend_from_slice(&67u16.to_be_bytes());
    out.extend_from_slice(&68u16.to_be_bytes());
    out.extend_from_slice(&(udp_len as u16).to_be_bytes());
    out.extend_from_slice(&[0, 0]); // UDP checksum is optional over IPv4
    out.extend_from_slice(&b);
    Some(out)
}

/// Answers an ARP request for the router address, so a console that probes its gateway finds one.
fn arp_reply(frame: &[u8]) -> Option<Vec<u8>> {
    if ethertype(frame) != 0x0806 || frame.len() < 42 {
        return None;
    }
    let arp = &frame[14..42];
    if arp[0..8] != [0, 1, 8, 0, 6, 4, 0, 1] || arp[24..28] != ROUTER_IP {
        return None;
    }
    let mut out = Vec::with_capacity(60);
    out.extend_from_slice(&arp[8..14]);
    out.extend_from_slice(&ROUTER_MAC);
    out.extend_from_slice(&[0x08, 0x06, 0, 1, 8, 0, 6, 4, 0, 2]);
    out.extend_from_slice(&ROUTER_MAC);
    out.extend_from_slice(&ROUTER_IP);
    out.extend_from_slice(&arp[8..18]); // the asker's MAC and IP
    out.resize(60, 0);
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frame(dst: [u8; 6], src: [u8; 6], ethertype: u16, payload: &[u8]) -> Vec<u8> {
        let mut f = Vec::new();
        f.extend_from_slice(&dst);
        f.extend_from_slice(&src);
        f.extend_from_slice(&ethertype.to_be_bytes());
        f.extend_from_slice(payload);
        f
    }

    const A: [u8; 6] = [2, 0, 0, 0, 0, 0xA];
    const B: [u8; 6] = [2, 0, 0, 0, 0, 0xB];
    const C: [u8; 6] = [2, 0, 0, 0, 0, 0xC];

    fn dhcp(kind: u8, mac: [u8; 6], requested: Option<[u8; 4]>) -> Vec<u8> {
        let mut b = vec![0u8; 240];
        b[0] = 1;
        b[1] = 1;
        b[2] = 6;
        b[4..8].copy_from_slice(&[0xDE, 0xAD, 0xBE, 0xEF]);
        b[28..34].copy_from_slice(&mac);
        b[236..240].copy_from_slice(&[99, 130, 83, 99]);
        b.extend_from_slice(&[53, 1, kind]);
        if let Some(ip) = requested {
            b.extend_from_slice(&[50, 4]);
            b.extend_from_slice(&ip);
        }
        b.push(255);
        let mut p = vec![0x45, 0, 0, 0, 0, 0, 0, 0, 64, 17, 0, 0, 0, 0, 0, 0, 255, 255, 255, 255];
        let udp_len = (8 + b.len()) as u16;
        p[2..4].copy_from_slice(&(20 + udp_len).to_be_bytes());
        p.extend_from_slice(&68u16.to_be_bytes());
        p.extend_from_slice(&67u16.to_be_bytes());
        p.extend_from_slice(&udp_len.to_be_bytes());
        p.extend_from_slice(&[0, 0]);
        p.extend_from_slice(&b);
        frame(BROADCAST, mac, 0x0800, &p)
    }

    #[test]
    fn floods_then_learns() {
        let mut s = Switch::new();
        let (a, b, c) = (s.join().unwrap(), s.join().unwrap(), s.join().unwrap());
        // B is unknown: A's frame floods to B and C.
        let out = s.on_frame(a, &frame(B, A, 0x0800, &[0; 46]));
        assert_eq!(out.iter().map(|d| d.0).collect::<Vec<_>>(), vec![b, c]);
        // B answers: A is known, so only A receives.
        let out = s.on_frame(b, &frame(A, B, 0x0800, &[0; 46]));
        assert_eq!(out.iter().map(|d| d.0).collect::<Vec<_>>(), vec![a]);
        // Broadcast from C reaches the other two, never C.
        let out = s.on_frame(c, &frame(BROADCAST, C, 0x0800, &[0; 46]));
        assert_eq!(out.iter().map(|d| d.0).collect::<Vec<_>>(), vec![a, b]);
        // A member that left no longer receives, and its address is forgotten.
        s.leave(b);
        assert_eq!(s.on_frame(a, &frame(B, A, 0x0800, &[0; 46])).iter().map(|d| d.0).collect::<Vec<_>>(), vec![c]);
    }

    #[test]
    fn drops_bad_frames_and_strangers() {
        let mut s = Switch::new();
        let a = s.join().unwrap();
        s.join().unwrap();
        assert!(s.on_frame(a, &[0; 13]).is_empty());
        assert!(s.on_frame(a, &vec![0; MAX_FRAME + 1]).is_empty());
        assert!(s.on_frame(5, &frame(BROADCAST, A, 0x0800, &[0; 46])).is_empty());
    }

    #[test]
    fn six_members_at_most() {
        let mut s = Switch::new();
        for expected in 0..MAX_PLAYERS {
            assert_eq!(s.join(), Some(expected));
        }
        assert_eq!(s.join(), None);
        s.leave(2);
        assert_eq!(s.join(), Some(2));
    }

    #[test]
    fn dhcp_offer_and_ack_use_the_slot_address() {
        let mut s = Switch::new();
        s.join().unwrap();
        let b = s.join().unwrap();
        for (kind, reply_kind) in [(1u8, 2u8), (3, 5)] {
            let out = s.on_frame(b, &dhcp(kind, B, (kind == 3).then(|| slot_ip(b))));
            assert_eq!(out.len(), 1, "the reply goes to the asker only");
            assert_eq!(out[0].0, b);
            let f = &out[0].1;
            assert_eq!(&f[6..12], &ROUTER_MAC);
            assert_eq!(checksum(&f[14..34]), 0, "IPv4 header checksum");
            let bootp = &f[42..];
            assert_eq!(bootp[0], 2);
            assert_eq!(&bootp[4..8], &[0xDE, 0xAD, 0xBE, 0xEF]);
            assert_eq!(&bootp[16..20], &[192, 168, 47, 12]);
            assert_eq!(&bootp[28..34], &B);
            assert_eq!(&bootp[240..243], &[53, 1, reply_kind]);
        }
        // A request for someone else's address is refused.
        let out = s.on_frame(b, &dhcp(3, B, Some([192, 168, 47, 11])));
        assert_eq!(&out[0].1[42 + 240..42 + 243], &[53, 1, 6]);
    }

    #[test]
    fn arp_for_the_router_is_answered() {
        let mut s = Switch::new();
        let a = s.join().unwrap();
        s.join().unwrap();
        let mut arp = vec![0, 1, 8, 0, 6, 4, 0, 1];
        arp.extend_from_slice(&A);
        arp.extend_from_slice(&slot_ip(a));
        arp.extend_from_slice(&[0; 6]);
        arp.extend_from_slice(&ROUTER_IP);
        let out = s.on_frame(a, &frame(BROADCAST, A, 0x0806, &arp));
        assert_eq!(out.len(), 1);
        let f = &out[0].1;
        assert_eq!(&f[0..6], &A);
        assert_eq!(&f[20..22], &[0, 2]);
        assert_eq!(&f[22..28], &ROUTER_MAC);
        assert_eq!(&f[28..32], &ROUTER_IP);
        // ARP between consoles is switched normally.
        arp[24..28].copy_from_slice(&slot_ip(1));
        assert_eq!(s.on_frame(a, &frame(BROADCAST, A, 0x0806, &arp))[0].0, 1);
    }
}
