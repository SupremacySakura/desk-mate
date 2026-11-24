PYTHON ?= python3
HDC ?= /Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc
EVIDENCE ?= evidence/2026-11-24-non-reference-dev
SERIAL ?=
DEVICE_ENTRY ?=
MEASUREMENT ?=

.PHONY: help test validate capture clean

help:
	@printf '%s\n' \
		'make test       Run the executable contract tests' \
		'make validate   Validate the committed pending report' \
		'make capture    Capture read-only evidence; requires SERIAL=...' \
		'make clean      Remove Python cache files'

test:
	$(PYTHON) -m unittest discover -s tests -v

validate:
	$(PYTHON) gate2a.py validate-report --report "$(EVIDENCE)/report.json"

capture:
	@test -n "$(SERIAL)" || (echo "SERIAL is required" >&2; exit 2)
	$(PYTHON) gate2a.py capture \
		--output "$(EVIDENCE)" \
		--device-serial "$(SERIAL)" \
		--hdc "$(HDC)" \
		$(if $(DEVICE_ENTRY),--device-entry "$(DEVICE_ENTRY)",) \
		$(if $(MEASUREMENT),--measurement "$(MEASUREMENT)",)

clean:
	find . -type d -name __pycache__ -prune -exec rm -rf {} +
	find . -type f -name '*.pyc' -delete
