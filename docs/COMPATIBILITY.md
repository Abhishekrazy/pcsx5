# Compatibility System

## Status model

Use explicit states:

- UNKNOWN
- DOES_NOT_BOOT
- BOOTS
- MENU
- IN_GAME
- PLAYABLE
- STABLE
- COMPLETE

"Launches" is not equivalent to "works."

## Database fields

Minimum fields:

- title_id
- title_name
- version
- tested_build
- emulator_commit
- status
- renderer
- resolution
- input_backend
- audio_backend
- save_state_tested
- last_tested
- failure_signature
- workaround_id
- evidence
- notes

## Workarounds

A workaround must be:
- named
- scoped
- documented
- testable
- removable

Do not encode compatibility workarounds as random conditionals.

## Regression policy

Every fixed compatibility bug should gain a regression artifact where feasible:
- unit test
- trace fixture
- loader fixture
- GPU command fixture
- save-state fixture
- boot test
- or documented manual test when automation is impossible

