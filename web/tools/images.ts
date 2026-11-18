// Decoding image files in the tools (image-diff.ts, the page checkers) with the web runtime's
// decoder, stb_image built to WebAssembly, so they read images as native image_diff.cpp's stbi_load
// does. ZGAL_DECODE_WASM names the module; by default it's this checkout's
// build/web-decode/stb_image.wasm (make build/web-decode/stb_image.wasm builds it).
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { ImageDecoder, type DecodedImage } from '../zgal/image-decode.ts';

let decoder: Promise<ImageDecoder> | undefined;

// An image file's pixels as RGBA8; throws with stb_image's reason when it can't be decoded
export async function decodeImageFile(file: Uint8Array): Promise<DecodedImage> {
  decoder ??= (async () => {
    const wasm = process.env.ZGAL_DECODE_WASM
      ? pathToFileURL(resolve(process.env.ZGAL_DECODE_WASM))
      : new URL('../../build/web-decode/stb_image.wasm', import.meta.url);
    try {
      return await ImageDecoder.load(wasm);
    } catch (error) {
      throw new Error(`${error instanceof Error ? error.message : String(error)} (make build/web-decode/stb_image.wasm, or set ZGAL_DECODE_WASM)`);
    }
  })();
  return (await decoder).decode(file);
}
