mod messages {
    struct MotorSetpoints {
        steering_pos: f32,
        rear_left_vel: f32,
        rear_right_vel: f32,
    }

    struct HostEmergency {}
}

struct ControlPacketHeader {
    version: u8,
    status: u8,
}
