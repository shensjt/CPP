PYTHON ?= python3

.PHONY: deps blog blog-local server kv test run run-local run-kv bench-kv

deps:
	$(PYTHON) -m pip install -r blog/requirements.txt

blog:
	$(PYTHON) blog/scripts/build.py

blog-local:
	$(PYTHON) blog/scripts/build.py --content docs/blog

server:
	cmake -S . -B build/server -DCMAKE_BUILD_TYPE=Release
	cmake --build build/server -j

kv:
	cmake -S . -B build/server -DCMAKE_BUILD_TYPE=Release
	cmake --build build/server --target kv_store -j

bench-kv:
	cmake -S . -B build/server -DCMAKE_BUILD_TYPE=Release
	cmake --build build/server --target bench_kv -j

test:
	cmake -S tests -B build/tests
	cmake --build build/tests -j
	ctest --test-dir build/tests --output-on-failure

run: blog server
	./build/server/web_server --host 127.0.0.1 --port 8080 \
		--root projects/web_server/www --threads 4 --timeout 60

run-local: blog-local server
	./build/server/web_server --host 127.0.0.1 --port 8080 \
		--root projects/web_server/www --threads 4 --timeout 60

run-kv: kv
	./build/server/kv_store --host 127.0.0.1 --port 6380 --threads 4
