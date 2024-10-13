use sb_ctrl_bridge::protocol::messages;
use sb_ctrl_bridge_impl::OutPacket;

fn main() {
    let motor_cmds = messages::MotorSetpoints {
        steering_pos: 1.0,
        rear_left_vel: -1.0,
        rear_right_vel: 1.0,
    };
    println!("message: {motor_cmds:?}");
    let mut packet_buf = [0u8; 1500];
    let mut packet_slice = &mut packet_buf[..];
    motor_cmds.serialize(&mut packet_slice).unwrap();
    let new_len = packet_slice.len();
    println!("binary form: {:?}", &packet_buf[0..1500 - new_len]);
}
