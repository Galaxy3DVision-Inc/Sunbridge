# Sunbridge

Sunbridge is an open bridge layer between Sunshine and external streaming systems.

It provides a small, focused interface for external processes to control Sunshine-side streaming operations and receive encoded media output through a simple bridge boundary. The project is intended to keep the Sunshine integration layer open while allowing external systems to communicate through a narrow command and media interface.

## Goals

- Provide an open integration layer for Sunshine-based streaming
- Keep the bridge boundary small and explicit
- Pass simple control commands such as display selection and stream configuration
- Return encoded media frames for external processing or transport
- Avoid exposing unnecessary internal implementation details across the bridge boundary

## Scope

Sunbridge is responsible for:

- interfacing with Sunshine on the open-source side
- accepting external control requests
- handling display selection and stream-related parameters
- returning encoded video frames and related stream output

Typical inputs include:

- display ID
- resolution
- stream control commands

Typical outputs include:

- encoded H.264 / AV1 video frames
- stream status and related events

## Design Principles

- Small and stable interface surface
- Clear separation between the Sunshine-side bridge and external systems
- Standardized command and media flow
- Minimal coupling across the bridge boundary
- Open-source implementation for the Sunshine integration layer

## Architecture Overview

Sunbridge sits between Sunshine and an external streaming system.

- Sunshine side: open bridge integration
- External side: command sender and encoded-frame consumer
- Bridge boundary: simple commands and encoded media packets

## License

This project is licensed under the GNU General Public License v3.0.

See the [LICENSE](LICENSE) file for details.

## Status

Early-stage bridge project for Sunshine-compatible streaming integration.
