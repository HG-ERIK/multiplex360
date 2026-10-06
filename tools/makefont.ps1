# Generates the app's bitmap fonts (media\Fonts\Text_16.fnt and Text_22.fnt) with every
# glyph the app needs, including Hungarian o/u with double acute.
#
#   .\tools\makefont.ps1                -> media\Fonts\Text_16.fnt and Text_22.fnt
#
# Glyphs are white with a soft black drop shadow; the app tints them with the text colour.
# File format (big-endian, read by src/Font.cpp):
#   'M3FN', u32 version (1),
#   u16 cellHeight, u16 lineAdvance, u16 textureWidth, u16 textureHeight,
#   u16 maxChar, u16 glyphCount, u16 translator[maxChar + 1] (char -> glyph, '?' if missing),
#   glyphCount x { u16 x, u16 y, u16 width, s16 offset, u16 advance },
#   textureWidth * textureHeight pixels as A, R, G, B bytes.
param([string]$FontFile = '')

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Add-Type -AssemblyName System.Drawing

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.IO;

public static class FontMaker
{
    class Glyph { public char C; public int X, Y, W; public int Offset, Advance; }

    static void BE16(BinaryWriter w, int v) { w.Write((byte)((v >> 8) & 255)); w.Write((byte)(v & 255)); }
    static void BE32(BinaryWriter w, uint v) { w.Write((byte)(v >> 24)); w.Write((byte)(v >> 16)); w.Write((byte)(v >> 8)); w.Write((byte)v); }

    public static string Make(string fontFile, float points, string path, char[] chars, int texW)
    {
        const int Shadow = 2, Pad = 2;
        using (PrivateFontCollection fonts = new PrivateFontCollection())
        {
        fonts.AddFontFile(fontFile);
        FontFamily family = fonts.Families[0];
        FontStyle style = family.IsStyleAvailable(FontStyle.Bold) ? FontStyle.Bold : FontStyle.Regular;
        using (Font font = new Font(family, points, style, GraphicsUnit.Point))
        using (Bitmap scratch = new Bitmap(256, 128, PixelFormat.Format32bppArgb))
        using (Graphics sg = Graphics.FromImage(scratch))
        {
            sg.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            StringFormat fmt = StringFormat.GenericTypographic;
            fmt.FormatFlags |= StringFormatFlags.MeasureTrailingSpaces;
            int lineH = (int)Math.Ceiling(font.GetHeight(sg));
            int cellH = lineH + Shadow + 1;

            // Measure ink bounds and advance of each glyph.
            List<Glyph> glyphs = new List<Glyph>();
            foreach (char c in chars)
            {
                sg.Clear(Color.Transparent);
                sg.DrawString(c.ToString(), font, Brushes.White, 32, 0, fmt);
                int left = int.MaxValue, right = -1;
                for (int y = 0; y < scratch.Height; y++)
                    for (int x = 0; x < scratch.Width; x++)
                        if (scratch.GetPixel(x, y).A > 8) { if (x < left) left = x; if (x > right) right = x; }
                float adv = sg.MeasureString(c.ToString(), font, 1000, fmt).Width;
                Glyph g = new Glyph();
                g.C = c;
                g.Advance = (int)Math.Round(adv);
                if (right < 0) { g.Offset = 0; g.W = 1; }         // space and friends
                else { g.Offset = left - 32; g.W = right - left + 1 + Shadow; }
                glyphs.Add(g);
            }

            // Pack rows into the texture.
            int px = 0, py = 0;
            foreach (Glyph g in glyphs)
            {
                if (px + g.W + Pad > texW) { px = 0; py += cellH + Pad; }
                g.X = px; g.Y = py;
                px += g.W + Pad;
            }
            int texH = 64;
            while (texH < py + cellH) texH *= 2;

            int maxChar = 0;
            foreach (Glyph g in glyphs) if (g.C > maxChar) maxChar = g.C;
            int fallback = glyphs.FindIndex(g => g.C == '?');

            using (Bitmap tex = new Bitmap(texW, texH, PixelFormat.Format32bppArgb))
            using (Graphics tg = Graphics.FromImage(tex))
            using (BinaryWriter w = new BinaryWriter(File.Create(path)))
            {
                tg.Clear(Color.Transparent);
                tg.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
                using (SolidBrush shadow = new SolidBrush(Color.FromArgb(150, 0, 0, 0)))
                    foreach (Glyph g in glyphs)
                    {
                        float x = g.X - g.Offset, y = g.Y;
                        tg.SetClip(new Rectangle(g.X, g.Y, g.W, cellH));
                        tg.DrawString(g.C.ToString(), font, shadow, x + Shadow, y + Shadow, fmt);
                        tg.DrawString(g.C.ToString(), font, Brushes.White, x, y, fmt);
                    }
                tg.ResetClip();

                w.Write(new byte[] { (byte)'M', (byte)'3', (byte)'F', (byte)'N' });
                BE32(w, 1);
                BE16(w, cellH); BE16(w, lineH); BE16(w, texW); BE16(w, texH);
                BE16(w, maxChar); BE16(w, glyphs.Count);
                int[] translator = new int[maxChar + 1];
                for (int i = 0; i <= maxChar; i++) translator[i] = fallback;
                for (int i = 0; i < glyphs.Count; i++) translator[glyphs[i].C] = i;
                for (int i = 0; i <= maxChar; i++) BE16(w, translator[i]);
                foreach (Glyph g in glyphs)
                {
                    BE16(w, g.X); BE16(w, g.Y); BE16(w, g.W); BE16(w, g.Offset & 0xFFFF); BE16(w, g.Advance);
                }
                BitmapData bits = tex.LockBits(new Rectangle(0, 0, texW, texH), ImageLockMode.ReadOnly,
                                               PixelFormat.Format32bppArgb);
                byte[] bgra = new byte[texW * texH * 4];
                System.Runtime.InteropServices.Marshal.Copy(bits.Scan0, bgra, 0, bgra.Length);
                tex.UnlockBits(bits);
                for (int i = 0; i < bgra.Length; i += 4)
                {
                    w.Write(bgra[i + 3]); w.Write(bgra[i + 2]); w.Write(bgra[i + 1]); w.Write(bgra[i]);   // A R G B
                }
            }
            return string.Format("{0}: {1} glyphs, line {2}px, texture {3}x{4}", Path.GetFileName(path),
                                 glyphs.Count, lineH, texW, texH);
        }
        }
    }
}
'@

$chars = New-Object System.Collections.Generic.List[char]
foreach ($r in @(@(32,126),@(160,383),@(8211,8212),@(8216,8222),@(8226,8226),@(8230,8230),@(8364,8364))) {
    for ($i = $r[0]; $i -le $r[1]; $i++) { $chars.Add([char]$i) }
}

if (-not $FontFile) { $FontFile = "$root\media\Fonts\source\Inter-Bold.ttf" }
foreach ($spec in @(@('Text_16', 15.0, 512), @('Text_22', 21.0, 512))) {
    [FontMaker]::Make($FontFile, $spec[1], "$root\media\Fonts\$($spec[0]).fnt", $chars.ToArray(), $spec[2])
}
