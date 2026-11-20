PYTHON ?= python3
MKDOCS := $(PYTHON) -m mkdocs

.PHONY: deps blog blog-local serve server kv test verify run run-local run-kv publish

deps:
	$(PYTHON) -m pip install -r blog/requirements.txt

blog:
	$(MKDOCS) build -f blog/mkdocs-demo.yml -d $(CURDIR)/build/www --strict

blog-local:
	$(MKDOCS) build -f blog/mkdocs.yml -d $(CURDIR)/build/www --strict

serve:
	$(MKDOCS) serve -f blog/mkdocs.yml

server:
	cmake -S . -B build/server -DCMAKE_BUILD_TYPE=Release
	cmake --build build/server -j

kv:
	cmake -S . -B build/server -DCMAKE_BUILD_TYPE=Release
	cmake --build build/server --target kv_store -j

test:
	cmake -S tests -B build/tests
	cmake --build build/tests -j
	ctest --test-dir build/tests --output-on-failure

# 本地和 CI 共用的验证入口：构建、单测、演示站构建。
verify:
	cmake -S . -B build/verify -DCMAKE_BUILD_TYPE=Release
	cmake --build build/verify -j
	cmake -S tests -B build/verify-tests
	cmake --build build/verify-tests -j
	ctest --test-dir build/verify-tests --output-on-failure
	$(MKDOCS) build -f blog/mkdocs-demo.yml -d $(CURDIR)/build/verify-www --strict

publish:
	./scripts/publish-content.sh

run: blog server
	./build/server/web_server --host 127.0.0.1 --port 8080 \
		--root build/www --threads 4 --timeout 60

run-local: blog-local server
	./build/server/web_server --host 127.0.0.1 --port 8080 \
		--root build/www --threads 4 --timeout 60

run-kv: kv
	./build/server/kv_store --host 127.0.0.1 --port 6380 --threads 4
