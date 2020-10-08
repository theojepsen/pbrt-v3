#!/usr/bin/env node

'use strict';

const http = require('http');
const url = require('url');

const range = (start, stop, step = 1) =>
  Array(Math.ceil((stop - start) / step))
    .fill(start)
    .map((x, y) => x + y * step);

const shuffle = (array) => {
  for (let i = array.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [array[i], array[j]] = [array[j], array[i]];
  }
}

class TileManager {
  constructor(width, height, tile_size) {
    this.width = width;
    this.height = height;
    this.tile_size = tile_size;

    this.finished_tiles = new Set();
    this.assigned_tiles = 0;
    this.tiles_x = Math.ceil(width / tile_size);
    this.tiles_y = Math.ceil(height / tile_size);
    this.total_tiles = this.tiles_x * this.tiles_y;
    this.remaining_tiles = range(0, this.total_tiles);
    shuffle(this.remaining_tiles);
  }

  next_tile() {
    if (this.remaining_tiles.length === 0) {
      return null;
    }

    const t = this.remaining_tiles.pop();
    this.assigned_tiles++
    return t;
  }

  tile_info(i) {
    const tile_x = i % this.tiles_x;
    const tile_y = Math.floor(i / this.tiles_y);
    const x0 = tile_x * this.tile_size;
    const y0 = tile_y * this.tile_size;
    const x1 = Math.min(x0 + this.tile_size, width);
    const y1 = Math.min(y0 + this.tile_size, height);

    return `${i} ${x0} ${x1} ${y0} ${y1}`;
  }

  got_tile(i) {
    this.finished_tiles.add(i);
  }
}

let usage = () => {
  console.log("coordinator.js PORT WIDTH HEIGHT MACHINES THREADS-PER-MACHINE");
  process.exit(1);
}

if (process.argv.length < 7) {
  usage();
}

let i = 2;
const port = parseInt(process.argv[i++])
const width = parseInt(process.argv[i++]);
const height = parseInt(process.argv[i++]);
const machines = parseInt(process.argv[i++]);
const threads = parseInt(process.argv[i++]);
const total_threads = machines * threads;
let active_threads = 0;

let tile_manager = new TileManager(width, height, 32);

console.log(`— image size: ${width}x${height}`);
console.log(`— cluster size: ${threads}x${machines}`);

const request_listener = (request, response) => {
  const d = url.parse(request.url, true);

  if (d.pathname === "/tile") {
    const tinfo = (active_threads < total_threads) ? null
      : tile_manager.tile_info(tile_manager.next_tile());

    response.statusCode = 200;
    response.end(tinfo);
  }
  else if (d.pathname === "/done") {
    const t = d.query.t;
    tile_manager.got_tile(t);
    response.statusCode = 200;
    response.end();
  }
  else if (d.pathname === "/hello") {
    active_threads++;
    response.statusCode = 200;
    response.end();
  }
  else {
    response.statusCode = 500;
    response.end();
  }

};

const print_status = () => {
  console.log(`active=${Math.ceil(10000 * active_threads / total_threads) / 100}%, assigned=${Math.ceil(10000 * tile_manager.assigned_tiles / tile_manager.total_tiles) / 100}%, finished=${Math.ceil(10000 * tile_manager.finished_tiles.size / tile_manager.total_tiles) / 100}%`);
};

setInterval(print_status, 2000);

const server = http.createServer(request_listener);
server.listen(port, "0.0.0.0");

console.log(`— listening on 0.0.0.0:${port}`);
