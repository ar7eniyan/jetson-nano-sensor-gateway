use pnet::datalink::NetworkInterface;

const IFNAME: &str = "enp4s0";

fn main() -> Result<(), String> {
    let interfaces: Vec<NetworkInterface> = pnet::datalink::interfaces();
    dbg!(&interfaces);

    let eth_if: &NetworkInterface = interfaces
        .iter()
        .find(|&interface| interface.name == IFNAME)
        .ok_or_else(|| format!("No interface named {}", IFNAME))?;
    dbg!(&eth_if);

    Ok(())
}
