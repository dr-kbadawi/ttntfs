// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 TechTag GmbH
//
// Render the repository's social preview: the 1280x640 card GitHub shows when
// the repository is linked from elsewhere. It is uploaded by hand under the
// repository's Settings > Social preview (there is no API for that), so this
// only writes the file. It uses AppKit, not PIL as make-icon.py does, because
// the text wants CoreText's kerning and SF Pro's display cut.
//
//   swift fskit/Design/make-social-preview.swift   -> fskit/Design/SocialPreview.png

import AppKit

let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
let iconURL = here.appendingPathComponent("../NTFS/Assets.xcassets/AppIcon.appiconset/icon_512x512@2x.png")
let outURL = here.appendingPathComponent("SocialPreview.png")

let W = 1280, H = 640
let width = CGFloat(W), height = CGFloat(H)

func rgb(_ hex: UInt32) -> NSColor {
    NSColor(srgbRed: CGFloat(hex >> 16 & 0xff) / 255, green: CGFloat(hex >> 8 & 0xff) / 255,
            blue: CGFloat(hex & 0xff) / 255, alpha: 1)
}
// The site's light palette (site/index.html).
let bg = rgb(0xffffff), bg2 = rgb(0xf5f6f8), ink = rgb(0x17191d)

// Opaque sRGB: some sites show a transparent card's background as black.
guard let ctx = CGContext(data: nil, width: W, height: H, bitsPerComponent: 8, bytesPerRow: 0,
                          space: CGColorSpace(name: CGColorSpace.sRGB)!,
                          bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue),
      let icon = NSImage(contentsOf: iconURL) else {
    fatalError("cannot set up drawing, or read \(iconURL.path)")
}
NSGraphicsContext.current = NSGraphicsContext(cgContext: ctx, flipped: false)
NSGraphicsContext.current?.imageInterpolation = .high

NSGradient(starting: bg2, ending: bg)!.draw(in: NSRect(x: 0, y: 0, width: width, height: height), angle: 90)

// The icon above, one line below, the pair centred on the card.
let line = NSAttributedString(string: "Kext-free read/write NTFS for macOS", attributes: [
    .font: NSFont.systemFont(ofSize: 56, weight: .semibold),
    .foregroundColor: ink,
    .kern: -0.6,
])

// The icon's canvas has 9% transparent margin on each side (make-icon.py), so
// the visible plate is 82% of what is drawn. Lay out by the plate.
let iconSide: CGFloat = 330
let plateInset = iconSide * 0.09
let plate = iconSide - 2 * plateInset
let gap: CGFloat = 52
let textHeight = line.size().height
let top = (height + plate + gap + textHeight) / 2          // top edge of the plate

let iconRect = NSRect(x: (width - iconSide) / 2, y: top - plate - plateInset,
                      width: iconSide, height: iconSide)
NSGraphicsContext.saveGraphicsState()
let shadow = NSShadow()
shadow.shadowColor = NSColor.black.withAlphaComponent(0.18)
shadow.shadowOffset = NSSize(width: 0, height: -14)
shadow.shadowBlurRadius = 40
shadow.set()
icon.draw(in: iconRect, from: .zero, operation: .sourceOver, fraction: 1)
NSGraphicsContext.restoreGraphicsState()

line.draw(at: NSPoint(x: (width - line.size().width) / 2, y: top - plate - gap - textHeight))

guard let image = ctx.makeImage(),
      let dest = CGImageDestinationCreateWithURL(outURL as CFURL, "public.png" as CFString, 1, nil) else {
    fatalError("cannot write \(outURL.path)")
}
CGImageDestinationAddImage(dest, image, nil)
guard CGImageDestinationFinalize(dest) else { fatalError("cannot write \(outURL.path)") }
print("wrote", outURL.standardizedFileURL.path)
