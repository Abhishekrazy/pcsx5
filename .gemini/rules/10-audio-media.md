# Audio / Media Governance

Audio and media are separate concerns.

Audio:
guest audio API
-> audio service
-> decoder/mixer
-> platform output

Media:
guest media API
-> media abstraction
-> FFmpeg/Bink2/other backend
-> presentation/audio integration

Third-party codec APIs must remain behind adapters.

Do not make FFmpeg/Bink2 types part of generic emulator contracts.

License/distribution implications must be documented before packaging changes.

