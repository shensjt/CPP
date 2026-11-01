PYTHON ?= python3

.PHONY: deps blog server test run

deps:
	$(PYTHON) -m pip install -r blog/requirements.txt

blog:
	$(PYTHON) blog/scripts/build.py

server:
	cmake -S . -B build/server -DCMAKE_BUILD_TYPE=Release
	cmake --build build/server -j

test:
	cmake -S tests -B build/tests
	cmake --build build/tests -j
	ctest --test-dir build/tests --output-on-failure

run: blog server
	./build/server/web_server --host 127.0.0.1 --port 8080 \
		--root projects/web_server/www --threads 4 --timeout 60
