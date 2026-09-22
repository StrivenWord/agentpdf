# AGENT PDF

**a Markdown-to-PDF pipeline comprised of C++ utilities**

This program unites three traditional C++ open source programs in order to provide yet another solution for the conversion of PDF documents into plain-text Markdown files, accomodating for OCR needs.

---

## Core dependencies

- [Poppler](https://poppler.freedesktop.org/)
- [Leptonica](http://www.leptonica.org/)
- [Tesseract](https://tesseractocr.org/)

## Proposition

The core feature is providing the full pipeline for PDF document conversion for academic use cases in a way that is open to real-time adjustment when utilized in an AI workflow, i.e., by an AI coding harness or desktop app, possibly as part of a skill. (Any such skill files or agent files are outside of the scope of this repository.) To accomplish this goal, the program implements an interactive console mode, similar to a REPL, which an AI agent could use to adjust parameters and re-run conversions quickly. It is also intended to be usable in a normal non-interactive CLI interface.

It is intended to be both completely open source and completely non-commercial.

Currently the codebase uses the **C++20** syntax and conventions.

### Distinguishing features

- Support for low end hardware (desktop/laptop computers)
- Multi-platform support through compilation
- Academic-first workflows, accommodating recurring patterns and OCR needs in journal articles
- Agentic workflows
- Interactive mode

## Platforms

The intention is that this program be as multi-platform as reasonably possible. As of the current commit, it has been built and tested on two environments:

- An Mac having an Intel i5 processor, running macOS Sonoma
- A Chromebook having an Intel Celeron N3350 processor, running Debian 13.5 Trixie

It should work on other platforms. Testing and compiling on other platforms would be a welcome contribution.

## Development

This program was coded using agentic tools, including Cursor and Claude Code, with various models. The agentic process that went in to making this code is not necessarily important to the project itself. There is no commitment to maintain a particular workflow in ongoing development. There is no commitment that the whole repository has been authored in the same way; for instance, this README file was not AI generated at all.

## Contributing

Contributions are welcome. Feel free to fork and issue pull requests. Make sure the pull requests are succinct and clear enough for a human reviewer at intermediate technical ability to understand without AI assistance. You may use any tools or workflows to contribute and edit code. Try to match the extant conventions, including the C++20 syntax, unless having a reason not to.

Feel free to contact the maintainer. Compiling and running the program on different hardware and operating systems would be very welcome, as would trying it in different AI workflows, apps, harnesses, etc.

Thanks.

