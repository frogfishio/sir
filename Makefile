.PHONY: build dist clean bump version

build:
	cmake -S . -B build
	cmake --build build

dist: build
	cmake --build build --target dist

clean:
	rm -rf build

version:
	@cat VERSION

# Bump patch version in ./VERSION (x.y.z -> x.y.(z+1)).
bump:
	@set -e; \
	ver=$$(cat VERSION | tr -d ' \t\r\n'); \
	case "$$ver" in \
	  *.*.*) ;; \
	  *) echo "VERSION must be in form x.y.z (got '$$ver')"; exit 1;; \
	esac; \
	new=$$(echo "$$ver" | awk -F. '{ if (NF!=3) exit 1; printf "%d.%d.%d\n", $$1, $$2, $$3+1 }'); \
	echo "$$new" > VERSION; \
	echo "bumped VERSION: $$ver -> $$new"
