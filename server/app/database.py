from __future__ import annotations

from collections.abc import Generator

from fastapi import Request
from sqlalchemy import create_engine
from sqlalchemy.orm import DeclarativeBase, Session, sessionmaker


class Base(DeclarativeBase):
    pass


class Database:
    def __init__(self, url: str):
        connect_args = {"check_same_thread": False} if url.startswith("sqlite") else {}
        self.engine = create_engine(
            url,
            pool_pre_ping=True,
            connect_args=connect_args,
        )
        self.session_factory = sessionmaker(
            bind=self.engine,
            expire_on_commit=False,
            autoflush=False,
        )

    def session(self) -> Session:
        return self.session_factory()


def get_session(request: Request) -> Generator[Session, None, None]:
    session = request.app.state.database.session()
    try:
        yield session
    finally:
        session.close()
