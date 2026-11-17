# Security policy

Magatama is a game runtime for desktop and mobile; it opens no network ports and sends no data. It does read a disc
image (ISO, CHD or ZSO) you give it and files in its own data folder, so bugs in those parsers (memory-safety issues in
CHD, ZSO or ISO9660 handling, path traversal in the disc importer) are what we care about most.

Please report a vulnerability **privately** with GitHub's *Report a vulnerability* button (Security tab) rather than a
public issue. Include the version, platform and steps to reproduce; never attach game data. We aim to acknowledge reports
within a week. Only the latest revision of the default branch is supported.
