# remaster_seedvr.py - teacher frames from SeedVR2 7B Sharp (ByteDance, Apache 2.0) in ComfyUI (P40).
# SeedVR2 is a one-step restoration model trained on real footage: it keeps the layout and adds
# photographic detail. The tile is first shrunk by SV_DOWN (default 0.5) and restored back to its size,
# so the output lines up pixel for pixel with the game tile (same scale as the LTX teacher).
# Usage: nnvenv/bin/python -I remaster_seedvr.py IMG [IMG...]   (names relative to RM_INPUT)
# Env: SV_DOWN (0.5), SV_MODEL, SV_COLOR (lab), SV_SEED, SV_TAG (suffix), RM_PORT (8190)
import json, os, sys, time, urllib.request

PORT = os.environ.get('RM_PORT', '8190')
def post(path, data):
    r = urllib.request.Request(f'http://127.0.0.1:{PORT}{path}', json.dumps(data).encode(), {'Content-Type': 'application/json'})
    return json.load(urllib.request.urlopen(r))
def get(path): return json.load(urllib.request.urlopen(f'http://127.0.0.1:{PORT}{path}'))

def graph(img, w, h, prefix):
    down = float(os.environ.get('SV_DOWN', 0.5))
    return {
        "1": {"class_type": "UNETLoader", "inputs": {"unet_name": os.environ.get('SV_MODEL', 'seedvr2_7b_sharp_fp16.safetensors'), "weight_dtype": "default"}},
        "2": {"class_type": "VAELoader", "inputs": {"vae_name": "seedvr2_ema_vae_fp16.safetensors"}},
        "3": {"class_type": "LoadImage", "inputs": {"image": img}},
        # degrade: shrink, then lanczos back to the tile size = SeedVR2's input (it restores in place)
        "4": {"class_type": "ImageScale", "inputs": {"image": ["3", 0], "upscale_method": "area", "width": int(w * down), "height": int(h * down), "crop": "disabled"}},
        "5": {"class_type": "ImageScale", "inputs": {"image": ["4", 0], "upscale_method": "lanczos", "width": w, "height": h, "crop": "disabled"}},
        "6": {"class_type": "SeedVR2Preprocess", "inputs": {"resized_images": ["5", 0]}},
        "7": {"class_type": "VAEEncodeTiled", "inputs": {"pixels": ["6", 0], "vae": ["2", 0], "tile_size": 512, "overlap": 128, "temporal_size": 64, "temporal_overlap": 8}},
        "8": {"class_type": "SeedVR2Conditioning", "inputs": {"model": ["1", 0], "vae_conditioning": ["7", 0]}},
        "9": {"class_type": "KSampler", "inputs": {"model": ["1", 0], "positive": ["8", 0], "negative": ["8", 1], "latent_image": ["7", 0],
              "seed": int(os.environ.get('SV_SEED', 2026)), "steps": 1, "cfg": 1.0, "sampler_name": "euler", "scheduler": "simple", "denoise": 1.0}},
        "10": {"class_type": "VAEDecodeTiled", "inputs": {"samples": ["9", 0], "vae": ["2", 0], "tile_size": 512, "overlap": 128, "temporal_size": 64, "temporal_overlap": 8}},
        "11": {"class_type": "SeedVR2PostProcessing", "inputs": {"images": ["10", 0], "original_resized_images": ["5", 0],
               "color_correction_method": os.environ.get('SV_COLOR', 'lab')}},
        "12": {"class_type": "SaveImage", "inputs": {"images": ["11", 0], "filename_prefix": prefix}},
    }

if __name__ == '__main__':
    from PIL import Image
    root = os.environ.get('RM_INPUT', 'input')
    for img in sys.argv[1:]:
        w, h = Image.open(os.path.join(root, img)).size
        w, h = w // 16 * 16, h // 16 * 16
        prefix = 'halo_seedvr/' + os.path.splitext(os.path.basename(img))[0] + os.environ.get('SV_TAG', '')
        t0 = time.time(); pid = post('/prompt', {"prompt": graph(img, w, h, prefix)})['prompt_id']
        while True:
            hist = get('/history/' + pid)
            if pid in hist:
                st = hist[pid].get('status', {})
                if st.get('status_str') == 'error': print(img, 'ERROR', json.dumps(st.get('messages', []))[-1500:]); break
                outs = [i['filename'] for o in hist[pid]['outputs'].values() for i in o.get('images', [])]
                print(img, '->', outs, '%.0f s' % (time.time() - t0), flush=True); break
            time.sleep(2)
