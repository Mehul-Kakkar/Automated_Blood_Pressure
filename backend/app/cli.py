"""Offline owner and bearer credential provisioning commands."""

import argparse
import hashlib
import secrets
from datetime import datetime, timezone
from uuid import UUID

from sqlalchemy import select, update

from app.db.session import get_session_factory
from app.models import Owner, OwnerCredential


def main() -> None:
    parser = argparse.ArgumentParser(description="Provision API owner credentials")
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("create-owner", help="Create an owner and print one bearer token")
    create.add_argument("display_name", help="Private operator label; do not use a patient name")
    rotate = commands.add_parser("rotate-credential", help="Revoke an owner's active tokens and print a replacement")
    rotate.add_argument("owner_id", type=UUID)
    args = parser.parse_args()

    factory = get_session_factory()
    plaintext = secrets.token_urlsafe(32)
    digest = hashlib.sha256(plaintext.encode("utf-8")).hexdigest()
    with factory() as db, db.begin():
        if args.command == "create-owner":
            owner = Owner(display_name=args.display_name)
            db.add(owner)
            db.flush()
        else:
            owner = db.scalar(select(Owner).where(Owner.id == args.owner_id))
            if owner is None:
                parser.error("owner_id does not exist")
            db.execute(
                update(OwnerCredential)
                .where(OwnerCredential.owner_id == owner.id, OwnerCredential.revoked_at.is_(None))
                .values(revoked_at=datetime.now(timezone.utc))
            )
        credential = OwnerCredential(owner_id=owner.id, token_sha256=digest)
        db.add(credential)
        db.flush()
        owner_id = owner.id
        credential_id = credential.id
    print(f"Owner ID: {owner_id}")
    print(f"Credential ID: {credential_id}")
    print("Bearer token (shown once; keep it in a secret manager):")
    print(plaintext)


if __name__ == "__main__":
    main()
