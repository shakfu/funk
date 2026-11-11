{-# LANGUAGE OverloadedStrings #-}
-- The 22 patches of Tidal's old/examples/cycles.tidal (tidalcycles/Tidal,
-- fefb1c3), in tidal-core 1.10.2's names, queried as tests/tidal/Oracle.hs
-- queries: the edits are in docs/dev/patch-port.md. tests/tidal/ports.fk
-- ports them to funk; tests/tidal/patches.sh matches the two.
import Sound.Tidal.Context
import Data.List (sort)
import qualified Data.Map as Map

tk :: Rational -> Integer
tk r = floor (r * 46126080)

fmt :: Double -> String
fmt x = show (round (x * 1e9) :: Integer)

rV :: Value -> String
rV v = case v of
  VF x -> "f" ++ fmt x
  VN (Note x) -> "f" ++ fmt x
  VI i -> "i" ++ show i
  VS x -> "s" ++ x
  VR x -> "f" ++ fmt (fromRational x)
  _ -> "?"

rC :: ValueMap -> String
rC m = concatMap (\(k, v) -> k ++ "=" ++ rV v ++ ";") (Map.toList m)

-- the onsets of cycles [0, k), each cycle sorted, one line per cycle
out :: String -> Int -> ControlPattern -> IO ()
out name k p = mapM_ one [0 .. k - 1]
  where
    one c = putStrLn (name ++ " " ++ show c ++ " " ++ show (sort $ map ev $ filter eventHasOnset $
              queryArc p (Arc (fromIntegral c) (fromIntegral c + 1))))
    ev e = let Arc a b = wholeOrPart e in (tk a, tk b, rC (value e))

main :: IO ()
main = do
  out "p0" 8 $ brak $ let x = "bd [sn [[sn bd] sn]]/3" in interlace (sound $ slow 3 $ x) (sound $ every 3 (append "[bd]*6") x)
    # speed "4"
  out "p1" 8 $ weave 4 (speed $ (1+) <$> sine) [density 4 $ every 5 ((0.25 <~) . rev) $ striate 16 $ sound "[bd sn/2]/2", sound "bd [~ hc]*3"]
    # speed "3"
  out "p2" 8 $ (stutter 2 (1/32) $ whenmod 8 6 (rev) $ density 4 $ striate 4 $ sound $ samples "[bd [sn]*3]/4" (slow 13 $ run 12))
    # speed "2"
  out "p3" 8 $ whenmod 4 2 rev $ spread' (striate . pure) "[8 3 4, 5]" $ sound "[[nn/6 nn/4, [nn/5 nn/2 nn/3]/2]/3, kurt/1]"
  out "p4" 8 $ sound (samples "bd*3 [bd sn]/3 [bd*12 bd*6]/3 cp" "[0 2 4]/3") # end (slow 5 $ sine) # speed ((+1) <$> tri)
  out "p5" 8 $ slow 2 $ stack [spin 6 $ spread' (slow . pure) "3 4 5 2" $ sound "[bd*5 ht*5, [sid/4]*3 [lt*2]/5 mt*3, [sid/3]*3]"] # speed "2"
  out "p6" 8 $ every 2 rev $ spread' (striate' 6 . pure) (((+0.1) . (*0.2)) <$> slow 32 sine) $ sound "[diphone2/6 diphone2]/4"
  out "p6.1" 2 $ density 16 $ spread' (striate' 32 . pure) (((+0.04) . (*0.08)) <$> slow 128 sine) $ sound (samples "amencutup" (slow 12 $ run 16))
  out "p6.2" 8 $ density 2 $ jux (rev . every 2 ((26/8) <~)) $ spread' (striate' 8 . pure) (((+0.04) . (*0.04)) <$> slow 4 sine) $ sound "breaks157"
  out "p7" 8 $ slow 2 $ spread' (striate . pure) "[4 2 3 8]/3" $ whenmod 4 2 (jux (rev)) $ sound "[[bd mt*3 bd [sn*8 bd] [lt*4 bd] [~ sn*4] [ht/5]*6 ht*2]]"
  out "p7.1" 8 $ slow 2 $ spread' (striate . pure) "[4 2 8 1]/3" $ jux palindrome $ sound "[jvbass*4 [jvbass/5]*8 ~ [jvbass/9]*6, drum*2 sn/2 [drum/2]*6 rave/3]"
  out "p8" 8 $ slow 8 $ every 4 (rev) $ jux (# speed "[2,3]") $ spread' (striate . pure) "[4 3 2, 6]" $ sound (samples "stab*4 sn" $ run 8)
  out "p9" 8 $ every 2 (density 2) $ jux (spread' (striate . pure) "[1 16 4 8]/3") $ sound (samples "[drum bd*12]" "[1 2 3 0]") # speed "[2 1.5 1]/4"
  out "p10" 8 $ density 2 $ jux (iter 32) $ sound "bass3/2 bass3/0" # (speed $ every 2 rev $ "[8 3 4]/3")
  out "p11" 8 $ superimpose ((# vowel "a") . iter 4) $ jux rev $ sound (samples "[amencutup*8 bass3*2,[jvbass*8]/3]" $ slow (3/5) $ run 12)
  out "p12" 8 $ (jux (spread' ((<~) . pure) (toRational <$> (slow 16 sine))) $ sound "bd*2 pluck/2 sn/2 [pluck/0 pluck/1]") # speed "1 2 3 2"
  out "p12.1" 8 $ (jux (spread' ((<~) . pure) (toRational <$> (slow 2 sine))) $ striate 2 $ sound (samples "odx*2" (slow 3 $ run 4)))
  out "p13" 8 $ let sinerat1 = fmap toRational sine in stack [smash 4 (map pure [1..16]) $ (jux (spread' ((<~) . pure) (slow 64 $ sinerat1)) $ sound $ samples "[bd bd*4, jvbass*8]" (slow 4 $ run 8)), sound "future/1"]
  out "p14" 8 $ jux (spread' (density . pure) ((*4) . (1-) <$> (slow 8 $ tri))) $ sound "bd drum*2 drum/2 [bass3/2,bd]" # speed (slow 8 $ (+1) <$> tri)
  out "p15" 8 $ (spread' (striate' 4 . pure) ((+0.2) <$> slow 8 sine)) $ jux rev $ sound (samples "[latibro*6,bd]/2" "0 7 4") # speed "[1 2 1.5]/4"
  out "p16" 8 $ slow 2 $ weave 16 (pan sine) $ map sound ["[[odx*4[odx*2odx*8],[hh*4,~odx/12]*4],[blue/1]/2~~[~[odx/13]*2]]", (1/16) ~> samples "jvbass*16" "2 2[3 4]1"]
  out "p17" 8 $ sound (samples "{casio ~ bd*3, bd*2 rm*3, numbers lt [mt sn, lt*3 voodoo] sn bass3*2}" ("{7 2, 3 1 5}")) # end rand
