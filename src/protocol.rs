pub mod messages {
    use sb_ctrl_bridge_derive::derive_out_packet;

    #[derive_out_packet(0x42, 0xA0)]
    #[derive(Debug)]
    pub struct MotorSetpoints {
        pub steering_pos: f32,
        pub rear_left_vel: f32,
        pub rear_right_vel: f32,
    }

    struct HostEmergency {}
}

struct ControlPacketHeader {
    version: u8,
    status: u8,
}
