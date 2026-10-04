# xIPs: public-release review

Review date: 2026-10-04. Scope: local source, locally reachable branches/tags,
third-party notices, asset provenance and the existing packaging rules.
This is an engineering review, not a legal clearance or a completed publication.

## Changes in this preparation

- The application license and attribution are stated in LICENSE and NOTICE.
- THIRD-PARTY-NOTICES.md distinguishes the application's license from its dependencies.
- Existing packaging/install rules include the new license and provenance records.
- Asset facts and unresolved provenance are recorded in ASSET-PROVENANCE.md.

## Outstanding provenance and distribution checks

- Confirm that the application code and assets may be published, including any
  employer/customer agreements. A single Git author is not proof of ownership.
- Resolve pending asset provenance entries. Confirm the desired treatment of
  the personal Gmail address present in existing commit metadata.
- The authenticated remote inventory is recorded below. Historical Actions
  logs have not been inspected; an inventory is not a content clearance.
- For public binary distribution, supply the corresponding sources and any
  required installation/relinking information for the actual Qt build, and
  review notices of the exact deployed Qt modules and their bundled dependencies.
  License texts and upstream links alone do not certify full LGPL compliance.
- Confirm redistribution rights for any optional SuiteApp/SuiteUi SDK or runtime
  actually bundled with a release. This project does not grant rights to them.

The initial preparation changed local source and notice files only. The
maintainer subsequently instructed that these changes be pushed and the
repository made public (see below). Existing history is retained. Recheck
changes made after this review.

## Local verification of this preparation

- License/provenance documents and their relative links were checked.
- Modified existing files passed git diff --check.
- Changed PowerShell packaging scripts passed the PowerShell parser.
- During initial preparation, no application build, full runtime regression,
  formal repackaging, push, history rewrite or visibility change was performed.

## Publication instruction and remote inventory

On 2026-10-04 the maintainer explicitly requested pushing the prepared changes
and making this repository public. This instruction does not establish the
provenance of assets marked pending in ASSET-PROVENANCE.md.

Authenticated GitHub API reads on the same date confirmed push and administrator
permissions and the following pre-publication inventory:

- 1 remote branch(es) and 8 tag(s); branch tips match the local remote refs.
- No issues or pull requests, GitHub releases or release attachments.
- 0 historical Actions runs and no retained Actions artifacts at the time of inspection.
- Historical Actions log contents have not been reviewed by this preparation.

The publication operation retains commit history and does not rebuild or replace
installed application packages. Publication completion is verified separately
after pushing these changes.
