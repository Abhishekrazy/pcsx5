# Debugging Governance

Debug from first divergence.

Required flow:

Reproduce
-> Capture
-> Normalize
-> Find first divergence
-> Minimize
-> Instrument
-> Hypothesize
-> Validate
-> Fix
-> Regression

Logs should answer:
- what
- where
- when
- title/version
- thread
- subsystem
- guest PC when available
- relevant IDs/addresses
- failure reason

Avoid logging hot paths at high volume without gating.

Never fix a crash by suppressing its signature.

