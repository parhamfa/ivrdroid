from __future__ import annotations

import argparse
import json
from pathlib import Path

from .config import load_settings
from .database import Database
from .prompt_service import PromptImportError, import_prompt


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="python -m app.manage")
    commands = parser.add_subparsers(dest="command", required=True)
    import_command = commands.add_parser(
        "import-prompt",
        help="Import a local prompt through the production validation pipeline.",
    )
    import_command.add_argument("--name", required=True)
    import_command.add_argument("--file", required=True, type=Path)
    import_command.add_argument("--actor", default="local-deployment")
    return parser


def main(argv: list[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    settings = load_settings()
    settings.validate_runtime()
    database = Database(settings.database_url)

    if arguments.command == "import-prompt":
        try:
            with database.session() as session:
                prompt = import_prompt(
                    session,
                    settings,
                    name=arguments.name,
                    source_path=arguments.file,
                    actor=arguments.actor,
                )
                session.commit()
                print(
                    json.dumps(
                        {
                            "id": prompt.id,
                            "name": prompt.name,
                            "version": prompt.version,
                            "content_hash": prompt.content_hash,
                            "size_bytes": prompt.size_bytes,
                            "duration_ms": prompt.duration_ms,
                        },
                        sort_keys=True,
                    ),
                )
        except PromptImportError as error:
            _parser().error(str(error))
        return 0

    return 2


if __name__ == "__main__":
    raise SystemExit(main())
