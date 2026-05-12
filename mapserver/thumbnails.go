package main

import (
	"image"
	"image/jpeg"
	_ "image/png" // register PNG decoder
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"

	"golang.org/x/image/draw"
)

// On-demand cropped + thumbnail generator for /tower-screenshots/.
//
// Originals (whatever the user dropped into the screenshots directory) are
// never served directly. Every request is satisfied from a cached, cropped
// variant in `<dir>/.cache/`:
//
//   URL                              → cache file               size      crop
//   /tower-screenshots/foo.jpg       → <dir>/.cache/foo.jpg     full      yes
//   /tower-screenshots/foo-thumb.jpg → <dir>/.cache/foo-thumb.jpg 25%     yes
//
// The cropping window strips D2's UI chrome that appears in every
// screenshot: 13% off the left and right, 1% off the top, 21% off the
// bottom (see cropFractions below). A per-cache-path mutex serializes
// generation so two simultaneous requests don't race the encoder.

const (
	thumbSuffix  = "-thumb"
	thumbScale   = 0.25
	thumbQuality = 80
	fullQuality  = 88
	cacheSubdir  = ".cache"
)

// Fractions of the source image to remove from each side. Tuned to the
// player's HUD layout in the D2 in-game screenshots — feel free to tweak
// from one place.
var cropFractions = struct {
	left, right, top, bottom float64
}{
	left:   0.13,
	right:  0.13,
	top:    0.01,
	bottom: 0.21,
}

var thumbLocks sync.Map // map[string]*sync.Mutex

func thumbLock(path string) *sync.Mutex {
	m, _ := thumbLocks.LoadOrStore(path, &sync.Mutex{})
	return m.(*sync.Mutex)
}

func screenshotsHandler(dir string) http.Handler {
	cacheDir := filepath.Join(dir, cacheSubdir)
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// r.URL.Path is already stripped of the mount prefix by
		// http.StripPrefix in main.go.
		name := filepath.Clean(strings.TrimPrefix(r.URL.Path, "/"))
		if name == "" || name == "." || name == cacheSubdir ||
			strings.HasPrefix(name, "..") ||
			strings.Contains(name, string(filepath.Separator)+"..") ||
			strings.HasPrefix(name, cacheSubdir+string(filepath.Separator)) {
			http.NotFound(w, r)
			return
		}
		ext := strings.ToLower(filepath.Ext(name))
		stem := strings.TrimSuffix(name, ext)

		// Compute the source filename and the cache filename + scale.
		isThumb := strings.HasSuffix(stem, thumbSuffix)
		var sourceStem string
		var scale float64
		if isThumb {
			sourceStem = strings.TrimSuffix(stem, thumbSuffix)
			scale = thumbScale
		} else {
			sourceStem = stem
			scale = 1.0
		}

		// Cache always lives at <cache>/<name>; cached files are JPEG.
		cachePath := filepath.Join(cacheDir, name)
		if fi, err := os.Stat(cachePath); err == nil && !fi.IsDir() {
			http.ServeFile(w, r, cachePath)
			return
		}

		// Locate the source on disk. We try a few extensions so the URL
		// can be a guess (foo.jpg URL while disk has foo.png, etc.).
		var srcPath string
		for _, e := range []string{ext, ".jpg", ".jpeg", ".png", ".webp"} {
			p := filepath.Join(dir, sourceStem+e)
			if fi, err := os.Stat(p); err == nil && !fi.IsDir() {
				srcPath = p
				break
			}
		}
		if srcPath == "" {
			http.NotFound(w, r)
			return
		}

		lock := thumbLock(cachePath)
		lock.Lock()
		defer lock.Unlock()
		// Re-check after locking — another goroutine may have just
		// produced the cache file while we were waiting.
		if fi, err := os.Stat(cachePath); err == nil && !fi.IsDir() {
			http.ServeFile(w, r, cachePath)
			return
		}
		if err := os.MkdirAll(cacheDir, 0o755); err != nil {
			http.Error(w, "cache mkdir: "+err.Error(), http.StatusInternalServerError)
			return
		}
		quality := fullQuality
		if isThumb {
			quality = thumbQuality
		}
		if err := generateVariant(srcPath, cachePath, scale, quality); err != nil {
			http.Error(w, "generate failed: "+err.Error(), http.StatusInternalServerError)
			return
		}
		http.ServeFile(w, r, cachePath)
	})
}

// Crop the source per cropFractions, optionally scale, encode JPEG. scale==1
// emits the cropped full-resolution version; scale<1 produces a thumb.
func generateVariant(srcPath, dstPath string, scale float64, quality int) error {
	src, err := os.Open(srcPath)
	if err != nil {
		return err
	}
	defer src.Close()
	img, _, err := image.Decode(src)
	if err != nil {
		return err
	}

	// Crop the bounded sub-rect. Most decoder outputs (RGBA, YCbCr) satisfy
	// SubImage(); we fall back to copying through an RGBA otherwise.
	cropR := cropRect(img.Bounds())
	type subImager interface {
		SubImage(r image.Rectangle) image.Image
	}
	var cropped image.Image
	if si, ok := img.(subImager); ok {
		cropped = si.SubImage(cropR)
	} else {
		rgba := image.NewRGBA(image.Rect(0, 0, cropR.Dx(), cropR.Dy()))
		draw.Draw(rgba, rgba.Bounds(), img, cropR.Min, draw.Src)
		cropped = rgba
	}

	// Resize if requested.
	var out image.Image = cropped
	if scale != 1.0 {
		newW := int(float64(cropR.Dx()) * scale)
		newH := int(float64(cropR.Dy()) * scale)
		if newW < 1 {
			newW = 1
		}
		if newH < 1 {
			newH = 1
		}
		rgba := image.NewRGBA(image.Rect(0, 0, newW, newH))
		draw.CatmullRom.Scale(rgba, rgba.Bounds(), cropped, cropped.Bounds(), draw.Over, nil)
		out = rgba
	}

	// Write atomically: tmp + rename so a crash mid-encode doesn't leave
	// a truncated file the next request would happily serve.
	tmp := dstPath + ".tmp"
	f, err := os.Create(tmp)
	if err != nil {
		return err
	}
	if err := jpeg.Encode(f, out, &jpeg.Options{Quality: quality}); err != nil {
		_ = f.Close()
		_ = os.Remove(tmp)
		return err
	}
	if err := f.Close(); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	return os.Rename(tmp, dstPath)
}

func cropRect(b image.Rectangle) image.Rectangle {
	w := float64(b.Dx())
	h := float64(b.Dy())
	x0 := b.Min.X + int(w*cropFractions.left)
	x1 := b.Max.X - int(w*cropFractions.right)
	y0 := b.Min.Y + int(h*cropFractions.top)
	y1 := b.Max.Y - int(h*cropFractions.bottom)
	if x1 <= x0 || y1 <= y0 {
		// Degenerate (cropFractions misconfigured) — return original.
		return b
	}
	return image.Rect(x0, y0, x1, y1)
}

