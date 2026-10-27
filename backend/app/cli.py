"""Offline owner and bearer credential provisioning commands."""

import argparse
import hashlib
import re
import secrets
from datetime import datetime, timezone

from sqlalchemy import select, update

from app.db.session import get_session_factory
from app.models import Owner, OwnerCredential


def main() -> None:
    parser = argparse.ArgumentParser(description="Provision API owner credentials")
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("create-owner", help="Create an owner and print one bearer token")
    create.add_argument("username", help="Unique lowercase account username (3-48 chars)")
    create.add_argument("display_name", help="Separate profile display name")
    rotate = commands.add_parser("rotate-credential", help="Revoke an owner's active tokens and print a replacement")
    rotate.add_argument("username", help="Account username")
    args = parser.parse_args()

    factory = get_session_factory()
    plaintext = secrets.token_urlsafe(32)
    digest = hashlib.sha256(plaintext.encode("utf-8")).hexdigest()
    with factory() as db, db.begin():
        if args.command == "create-owner":
            username = args.username.strip().lower()
            if not re.fullmatch(r"[a-z0-9_.-]{3,48}", username):
                parser.error("username must be 3-48 lowercase letters, digits, dots, underscores or hyphens")
            if db.get(Owner, username) is not None:
                parser.error("username is already registered")
            owner = Owner(username=username, display_name=args.display_name)
            db.add(owner)
            db.flush()
        else:
            owner = db.scalar(select(Owner).where(Owner.username == args.username.strip().lower()))
            if owner is None:
                parser.error("username does not exist")
            db.execute(
                update(OwnerCredential)
                .where(OwnerCredential.owner_username == owner.username, OwnerCredential.revoked_at.is_(None))
                .values(revoked_at=datetime.now(timezone.utc))
            )
        credential = OwnerCredential(owner_username=owner.username, token_sha256=digest)
        db.add(credential)
        db.flush()
        username = owner.username
        credential_id = credential.id
    print(f"Username: {username}")
    print(f"Credential ID: {credential_id}")
    print("Bearer token (shown once; keep it in a secret manager):")
    print(plaintext)


if __name__ == "__main__":
    main()
