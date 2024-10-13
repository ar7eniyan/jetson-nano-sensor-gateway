extern crate proc_macro;
use proc_macro::TokenStream;

#[proc_macro_attribute]
pub fn derive_out_packet(args: TokenStream, input: TokenStream) -> TokenStream {
    sb_ctrl_bridge_impl::derive_out_packet_impl(args, input)
}
