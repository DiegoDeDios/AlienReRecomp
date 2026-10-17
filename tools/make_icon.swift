// Draws the port's app icon (original artwork in the launcher's style:
// dark tile, green glow line, "AR" monogram) as a 1024x1024 PNG.
//   swift tools/make_icon.swift packaging/icon/icon_1024.png
import AppKit

let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "icon_1024.png"
let size = 1024
guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size,
                                 bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                                 colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0) else { exit(1) }
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
let ctx = NSGraphicsContext.current!.cgContext

func rgb(_ r: Int, _ g: Int, _ b: Int, _ a: CGFloat = 1) -> NSColor {
    NSColor(deviceRed: CGFloat(r) / 255, green: CGFloat(g) / 255, blue: CGFloat(b) / 255, alpha: a)
}
let green = rgb(104, 230, 120)

// tile (macOS icon grid: 824 px square, 185 px corner radius)
let tile = NSRect(x: 100, y: 100, width: 824, height: 824)
let path = NSBezierPath(roundedRect: tile, xRadius: 185, yRadius: 185)
ctx.saveGState()
path.addClip()
NSGradient(starting: rgb(5, 8, 7), ending: rgb(22, 31, 27))!.draw(in: tile, angle: 90)
// glow band across the middle
let bandY: CGFloat = 430
for (h, a) in [(CGFloat(120), CGFloat(0.10)), (CGFloat(56), CGFloat(0.28)), (CGFloat(18), CGFloat(0.65))] {
    let band = NSRect(x: tile.minX, y: bandY - h / 2, width: tile.width, height: h)
    NSGradient(colors: [green.withAlphaComponent(0), green.withAlphaComponent(a),
                        green.withAlphaComponent(a), green.withAlphaComponent(0)],
               atLocations: [0, 0.3, 0.7, 1], colorSpace: .deviceRGB)!.draw(in: band, angle: 0)
}
NSGradient(colors: [green.withAlphaComponent(0), rgb(220, 255, 228), green.withAlphaComponent(0)],
           atLocations: [0.05, 0.5, 0.95], colorSpace: .deviceRGB)!
    .draw(in: NSRect(x: tile.minX, y: bandY - 3, width: tile.width, height: 6), angle: 0)
ctx.restoreGState()
rgb(104, 230, 120, 0.35).setStroke()
path.lineWidth = 4
path.stroke()

func text(_ s: String, font: String, size: CGFloat, kern: CGFloat, color: NSColor, centerY: CGFloat) {
    let f = NSFont(name: font, size: size) ?? NSFont.boldSystemFont(ofSize: size)
    let shadow = NSShadow()
    shadow.shadowColor = green.withAlphaComponent(0.55)
    shadow.shadowBlurRadius = 28
    let attrs: [NSAttributedString.Key: Any] = [.font: f, .foregroundColor: color, .kern: kern, .shadow: shadow]
    let a = NSAttributedString(string: s, attributes: attrs)
    let b = a.boundingRect(with: NSSize(width: 2000, height: 2000), options: [])
    a.draw(at: NSPoint(x: 512 - (b.width - kern) / 2, y: centerY - b.height / 2 - b.origin.y / 2))
}
text("AR", font: "DINCondensed-Bold", size: 430, kern: 24, color: rgb(226, 234, 229), centerY: 600)
text("RECOMP", font: "DINAlternate-Bold", size: 88, kern: 26, color: green, centerY: 300)

NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: out))
print("wrote \(out)")
