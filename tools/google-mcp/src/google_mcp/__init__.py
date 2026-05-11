"""google-mcp — personal Gmail + Calendar MCP for the oc-personal-runner.

Surface is deliberately narrow: read freely, drafts (no send) for Gmail,
events (read + create) for Calendar. Anything that would mutate without
human review (sending, deleting, labels) is out.

See `oauth_cli` for the one-time consent flow that mints the token file
the server reads at startup.
"""
