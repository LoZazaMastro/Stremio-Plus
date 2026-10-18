// Offline episode identity and timestamp provenance for progressive playback.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "download_metadata.h"
#include "playback_progress.h"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
}

int main() {
    Meta series;
    series.type = "series"; series.has_scheduled_videos = true;
    Video selected;
    selected.id = "opaque/episode:selected"; selected.season = 3; selected.episode = 7;
    selected.title = "A title with · punctuation";
    series.videos.push_back(selected);
    check(!download_is_live(series, "series"), "scheduled series episodes are finite downloads");
    check(download_is_live(series, "tv"), "actual live television remains excluded");
    series.is_live = true;
    check(download_is_live(series, "series"), "explicit live metadata is respected for every type");
    series.is_live = false;
    DownloadRequest request;
    request.type = "series"; request.media_id = "series:opaque"; request.video_id = selected.id;
    download_episode_metadata(request, series);
    check(request.season == 3 && request.episode == 7 && request.episode_title == selected.title &&
          request.subtitle == "S03E07 · A title with · punctuation", "opaque episode metadata creates its full display label");
    request = {}; request.type = "series"; request.media_id = "tt1234567"; request.video_id = "tt1234567:0:2";
    download_episode_metadata(request, {});
    check(request.season == 0 && request.episode == 2 && request.subtitle == "S00E02",
          "partial metadata can recover specials from an exact standard episode ID");
    request = {}; request.type = "series"; request.media_id = "tt1234567"; request.video_id = "other:8:9";
    download_episode_metadata(request, {});
    check(request.season == -1 && request.episode == -1, "unrelated opaque identifiers never invent episode numbers");
    request.video_id = "tt1234567:2:4:extra";
    download_episode_metadata(request, {});
    check(request.season == -1 && request.episode == -1, "a trailing ID component cannot be mistaken for an episode number");
    request.video_id = "tt1234567:21474836480:4";
    download_episode_metadata(request, {});
    check(request.season == -1 && request.episode == -1, "overflowing episode metadata is rejected");
    check(download_legacy_episode_title(3, 7, "S3 · E7 · A title with · punctuation") == selected.title,
          "legacy labels retain the complete episode title including separators");
    check(download_legacy_episode_title(3, 7, "S03E07 · A title with · punctuation") == selected.title,
          "current labels retain the complete episode title");
    check(video_progress_key("series", "a:b", "c") != video_progress_key("series", "a", "b:c"),
          "opaque IDs with separators cannot collide in bookmark keys");
    check(video_progress_key("series", "a", "episode1") != video_progress_key("series", "a", "episode2"),
          "episodes of one series keep independent bookmarks");
    check(video_progress_key("series", "a", "video") != video_progress_key("movie", "a", "video"),
          "different provider media types do not share a bookmark");
    Progress bookmark;
    bookmark.video_id = selected.id; bookmark.time = 127.5; bookmark.duration = 1800;
    bookmark.download_id = "download-a"; bookmark.download_source = "source-a";
    bookmark.download_prefix = 8000000; bookmark.download_total = 100000000;
    check(resumable_progress(bookmark, selected.id), "a real finite saved playback point can be offered");
    check(!resumable_progress(bookmark, "another episode"), "another episode cannot use the saved point");
    check(progressive_resume_proven(bookmark, "download-a", "source-a", 8000000, 100000000),
          "the exact previously viewed prefix admits its proven timestamp");
    check(progressive_resume_proven(bookmark, "download-a", "source-a", 25000000, 100000000),
          "a growing verified prefix preserves the previously viewed timestamp");
    check(!progressive_resume_proven(bookmark, "download-a", "source-a", 7999999, 100000000),
          "checkpoint rollback below the saved byte bound refuses progressive resume");
    check(!progressive_resume_proven(bookmark, "download-b", "source-a", 8000000, 100000000),
          "another download of the same video cannot borrow a prefix proof");
    check(!progressive_resume_proven(bookmark, "download-a", "source-b", 8000000, 100000000),
          "a changed torrent or URL source invalidates the old prefix proof");
    check(!progressive_resume_proven(bookmark, "download-a", "source-a", 8000000, 99999999),
          "a changed resolved file length invalidates the prefix proof");
    bookmark.download_prefix = 0;
    check(!progressive_resume_proven(bookmark, "download-a", "source-a", 100000000, 100000000),
          "legacy bookmarks without presentation provenance cannot authorize a partial seek");
    bookmark.time = std::numeric_limits<double>::quiet_NaN();
    check(!resumable_progress(bookmark, selected.id), "invalid numeric positions cannot enter the player");
    bookmark.time = 1800;
    check(!resumable_progress(bookmark, selected.id), "a completed position is not offered as a resume point");
    std::cout << "Download bookmarks: " << checks << " checks passed\n";
}
