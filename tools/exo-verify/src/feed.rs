use anyhow::Result;

#[derive(clap::Args)]
pub struct FeedArgs {}

pub fn serve_forever(_: FeedArgs) -> Result<()> {
    anyhow::bail!("not yet")
}
