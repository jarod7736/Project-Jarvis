# Project Jarvis

A personal AI assistant framework — inspired by Tony Stark's JARVIS — designed to automate tasks, answer questions, and interact with external services through a conversational interface.

## Features

- Natural language understanding powered by large language models
- Tool / function-calling support for real-world integrations (calendars, email, search, etc.)
- Modular skill / plugin architecture — add new capabilities without touching core logic
- Memory layer for persistent context across conversations
- Voice input/output support (optional)
- REST API for embedding Jarvis in other applications

## Getting Started

### Prerequisites

- Python 3.11+
- An Anthropic or OpenAI API key

### Installation

```bash
git clone https://github.com/jarod7736/project-jarvis.git
cd project-jarvis
python -m venv .venv
source .venv/bin/activate   # Windows: .venv\Scripts\activate
pip install -r requirements.txt
```

### Configuration

Copy the example environment file and fill in your credentials:

```bash
cp .env.example .env
```

| Variable | Description |
|---|---|
| `ANTHROPIC_API_KEY` | Anthropic API key (required if using Claude) |
| `OPENAI_API_KEY` | OpenAI API key (required if using GPT) |
| `JARVIS_MODEL` | Model to use (default: `claude-sonnet-4-6`) |

### Running

```bash
python -m jarvis
```

## Project Structure

```
project-jarvis/
├── jarvis/
│   ├── core/          # Conversation loop, memory, tool dispatch
│   ├── skills/        # Individual capability modules
│   ├── integrations/  # Third-party service connectors
│   └── api/           # REST API server
├── tests/
├── .env.example
└── requirements.txt
```

## Adding a Skill

Create a new file in `jarvis/skills/` that implements the `Skill` interface:

```python
from jarvis.core import Skill, tool

class MySkill(Skill):
    @tool(description="Do something useful")
    def my_tool(self, input: str) -> str:
        return f"Result: {input}"
```

Jarvis auto-discovers skills on startup.

## Contributing

1. Fork the repo and create a feature branch (`git checkout -b feat/my-feature`)
2. Commit your changes with clear messages
3. Open a pull request describing what and why

## License

MIT
