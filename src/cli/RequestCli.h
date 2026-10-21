#pragma once
#include <QCommandLineParser>
#include <optional>
namespace xips::cli
{
void addRequestOptions(QCommandLineParser &parser);
std::optional<int> runRequest(const QCommandLineParser &parser);
} // namespace xips::cli
