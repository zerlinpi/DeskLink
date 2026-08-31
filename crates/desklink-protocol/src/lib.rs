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
