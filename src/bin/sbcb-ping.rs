use std::io::Write;

use sb_ctrl_bridge::{perror_fmt, EthernetComms};

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
        .ok_or("The third command line argument must be the network interface to use")?;
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

    args.get(4)
        .and_then(|path| {
            std::fs::File::create_new(path)
                .inspect_err(|e| {
                    println!("Error opening the stats file: {e}");
                })
                .ok()
        })
        .map(|mut stats_file| {
            stats_file
                .write_all(
                    rtt_times_ms
                        .iter()
                        .map(ToString::to_string)
                        .collect::<Vec<_>>()
                        .join(",")
                        .as_bytes(),
                )
                .inspect_err(|e| {
                    println!("Error writing stats to the file: {e}");
                })
                .ok()
        });

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
