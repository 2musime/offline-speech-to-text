#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringList>

// Merging of overlapping streaming partials. Kept free of widgets so it can be
// exercised on its own.
namespace partial_text {

// Compared on letters and digits only, so punctuation and capitalisation
// differences between windows do not break a match.
inline QString normalise(const QString& word) {
    QString stripped;
    stripped.reserve(word.size());
    for (const QChar character : word) {
        if (character.isLetterOrNumber()) {
            stripped.append(character.toLower());
        }
    }
    return stripped;
}

inline QStringList split_words(const QString& text) {
    return text.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
}

// Longest run where the tail of `existing` equals the head of `incoming`.
// Checked longest-first so the largest overlap wins.
inline int overlap_length(const QStringList& existing, const QStringList& incoming, int maximum) {
    const int limit = qMin(qMin(existing.size(), incoming.size()), maximum);
    for (int length = limit; length > 0; --length) {
        bool matches = true;
        for (int i = 0; i < length; ++i) {
            if (normalise(existing.at(existing.size() - length + i)) != normalise(incoming.at(i))) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return length;
        }
    }
    return 0;
}

// Appends `incoming` to `existing`, dropping the overlapping head. Trims from
// the front past `maximum_words` so a long session cannot grow without bound.
inline void append_with_overlap(
    QStringList& existing,
    const QString& incoming,
    int maximum_overlap,
    int maximum_words,
    bool& trimmed) {
    const QStringList words = split_words(incoming);
    if (words.isEmpty()) {
        return;
    }
    if (existing.isEmpty()) {
        existing = words;
    } else {
        for (int i = overlap_length(existing, words, maximum_overlap); i < words.size(); ++i) {
            existing.append(words.at(i));
        }
    }
    if (existing.size() > maximum_words) {
        existing = existing.mid(existing.size() - maximum_words);
        trimmed = true;
    }
}

}  // namespace partial_text
