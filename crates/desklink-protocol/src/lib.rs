macro_rules! generation_type {
    ($name:ident) => {
        #[repr(transparent)]
        #[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
        pub struct $name(u64);

        impl $name {
            pub const fn initial() -> Self {
                Self(1)
            }

            pub const fn get(self) -> u64 {
                self.0
            }

            pub const fn next(self) -> Option<Self> {
                match self.0.checked_add(1) {
                    Some(value) => Some(Self(value)),
                    None => None,
                }
            }
        }
    };
}

generation_type!(SessionGeneration);
generation_type!(PeerGeneration);
generation_type!(ControlChannelGeneration);
generation_type!(PointerChannelGeneration);
generation_type!(OperationGeneration);

#[cfg(test)]
mod tests {
    use super::{PeerGeneration, SessionGeneration};

    #[test]
    fn generations_are_strongly_typed_and_monotonic() {
        let session = SessionGeneration::initial();
        let peer = PeerGeneration::initial();

        assert_eq!(session.get(), 1);
        assert_eq!(session.next().expect("session generation increment").get(), 2);
        assert_eq!(peer.get(), 1);
        assert_eq!(peer.next().expect("peer generation increment").get(), 2);

        let _: SessionGeneration = session;
        let _: PeerGeneration = peer;
    }
}
