from sandover import levelinfo
from sandover.paths import root


def test_level_skies_parse(tmp_path):
    f = tmp_path / "level-info.gc"
    f.write_text("(define a\n  (new 'static 'level-load-info\n       :name 'jungle\n       :sky #t\n))\n"
                 "(define b\n       :name 'robocave\n       :sky #f\n)\n")
    assert levelinfo.level_skies(f) == {"jungle": True, "robocave": False}


def test_sky_inc_matches_level_info():
    r = root()
    skies = levelinfo.level_skies(r / levelinfo.LEVEL_INFO)
    assert skies["village1"] and skies["jungle"] and not skies["robocave"] and not skies["jungleb"]
    assert (r / levelinfo.SKY_INC).read_text(encoding="utf-8") == levelinfo.sky_inc(skies)
