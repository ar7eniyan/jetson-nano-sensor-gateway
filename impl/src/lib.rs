extern crate proc_macro;

use proc_macro::TokenStream;
use quote::{quote, ToTokens};
use syn::punctuated::Punctuated;

pub trait OutPacket {
    fn serialize(&self, sink: &mut impl std::io::Write) -> std::io::Result<()>;
}

pub fn derive_out_packet_impl(args: TokenStream, mut input: TokenStream) -> TokenStream {
    // Parse arguments (message class and type, for example "0x42, 0x69")
    let args_list = syn::parse_macro_input!(args with
        Punctuated<syn::LitInt, syn::Token![,]>::parse_separated_nonempty
    );
    if args_list.len() != 2 {
        panic!("Expected exactly 2 comma-separated integer literals");
    }
    let message_class = args_list[0].base10_parse::<u8>().unwrap();
    let message_type = args_list[1].base10_parse::<u8>().unwrap();
    let dump_header_code = quote! {
        sink.write_all(&#message_class.to_be_bytes())?;
        sink.write_all(&#message_type.to_be_bytes())?;
    };

    let input_clone = input.clone();
    let struct_ast = syn::parse_macro_input!(input_clone as syn::ItemStruct);
    let struct_name = struct_ast.ident;
    let struct_fields = match struct_ast.fields {
        syn::Fields::Named(fields_named) => fields_named.named,
        _ => panic!(
            "The definition of struct {} must contain a list of named fields",
            struct_name
        ),
    };
    let dump_fields_code = struct_fields.into_iter().map(|field: syn::Field| {
        const NUMERICAL_TYPES: [&str; 10] = [
            "u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64", "f32", "f64",
        ];

        let name = field
            .ident
            .as_ref()
            .expect("All of the struct fields are expected to have names");
        let name_string = field.ident.to_token_stream().to_string();
        let ty_string = field.ty.to_token_stream().to_string();

        if NUMERICAL_TYPES.contains(&ty_string.as_str()) {
            quote! {
                sink.write_all(&self.#name.to_be_bytes())?;
            }
        } else {
            panic!("Unserializable field {name_string} of type {ty_string}");
        }
    });

    let impl_code: TokenStream = quote! {
        impl ::sb_ctrl_bridge_impl::OutPacket for #struct_name {
            fn serialize(&self, mut sink: &mut impl std::io::Write) -> std::io::Result<()> {
                #dump_header_code
                #(#dump_fields_code)*
                Ok(())
            }
        }
    }
    .into();

    input.extend(impl_code);
    eprintln!("{input:}");
    input
}
