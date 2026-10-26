"""Persist a deterministic report snapshot for each report version."""
from typing import Sequence, Union
from alembic import op
import sqlalchemy as sa
revision: str = "0004_report_snapshot"
down_revision: Union[str, None] = "0003_owners_events"
branch_labels: Union[str, Sequence[str], None] = None
depends_on: Union[str, Sequence[str], None] = None
def upgrade() -> None:
    op.add_column("reports", sa.Column("report_data", sa.JSON(), nullable=True))
    op.add_column("reports", sa.Column("content_sha256", sa.String(length=64), nullable=True))
def downgrade() -> None:
    op.drop_column("reports", "content_sha256")
    op.drop_column("reports", "report_data")
