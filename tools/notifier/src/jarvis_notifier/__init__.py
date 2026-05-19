"""jarvis-notifier — proactive output channel for Project Jarvis (Sprint 1).

Translates priority-tagged notification requests into device-side TTS (via
MQTT to ``jarvis/speak``) and/or phone push (via Pushover), with a disk-
backed queue for the medium tier that drains when the device next goes
idle.
"""

__version__ = "0.1.0"
