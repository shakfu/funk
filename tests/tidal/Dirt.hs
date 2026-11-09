{-# LANGUAGE OverloadedStrings #-}
-- The cases of tests/tidal/dirt.cases, as Tidal 1.10.3's superdirtTarget
-- sends them: each onset of cycles [0, 4) at 0.5 cps through Tidal's own
-- toOSC, as d1 plays it, a float printed as its bits. An event's times are
-- in microseconds, to the nearest, as Link's are (assumed: Link's llround),
-- and delta is their difference.
-- tests/tidal/dirt.sh diffs this against funk-midi --dirt.
import Data.Int (Int64)
import Data.List (sortOn)
import qualified Sound.Osc.Fd as O
import GHC.Float (castFloatToWord32)
import Sound.Tidal.Context
import Sound.Tidal.Stream.Process (ProcessedEvent (..), toOSC)
import Sound.Tidal.Stream.Target (superdirtShape)
import Text.Printf (printf)

datum :: O.Datum -> String
datum (O.Int32 i) = "i:" ++ show i
datum (O.Float f) = printf "f:%08x" (castFloatToWord32 f)
datum (O.AsciiString s) = "s:" ++ O.ascii_to_string s
datum d = show d

pairs :: [O.Datum] -> String
pairs (k : v : rest) = " " ++ O.ascii_to_string (O.d_ascii_string k) ++ "=" ++ datum v ++ pairs rest
pairs _ = ""

-- a cycle's time in microseconds at 0.5 cps, to the nearest
us :: Rational -> Int64
us c = floor (c * 2000000 + 1 / 2)

out :: String -> ControlPattern -> IO ()
out name pat = mapM_ line es
  where
    es = sortOn (start . part) $ filter eventHasOnset $ queryArc (pat # pS "_id_" (pure "1")) (Arc 0 4)
    pe e = ProcessedEvent
      { peHasOnset = True, peEvent = e, peCps = 0.5,
        peDelta = us (stop (wholeOrPart e)) - us (start (wholeOrPart e)),
        peCycle = start (wholeOrPart e), peOnWholeOrPart = 0, peOnWholeOrPartOsc = 0, peOnPart = 0, peOnPartOsc = 0 }
    line e = mapM_ (\(_, _, O.Message p ds) -> putStrLn (name ++ " " ++ p ++ pairs ds)) (toOSC Nothing (pe e) superdirtShape)

main :: IO ()
main = do
  out "basic" (s "bd*2 [sn cp]" # n "<0 3>")
  out "kinds" (s "x" # cut "1 2" # vowel "a" # orbit 1)
  out "sound" (sound "bd:3 sn:1 [hh hh:2]")
  out "notes" (s "superpiano" # note "c e g a5")
  out "floats" (s "x*4" # pan sine # gain 1.2 # speed "-1" # legato 0.5)
  out "nos" (n "0 1")
  out "stack" (stack [s "a*2", s "b" # n 1])
  out "own" (s "x" # pF "delta" 0.5 # pF "cycle" 9 # pF "cps" 2)
  out "ownid" (s "x" # pS "_id_" "7")
  out "slow" (slow 2 $ s "long" # n "<1 2>")
  out "every" (every 2 (fast 2) $ s "bd sn" # crush 4)
  out "cont" (s "x*8" # cutoff (range 200 2000 saw) # resonance perlin)
