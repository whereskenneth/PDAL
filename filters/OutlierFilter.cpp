/******************************************************************************
 * Copyright (c) 2016-2017, Bradley J Chambers (brad.chambers@gmail.com)
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following
 * conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of Hobu, Inc. or Flaxen Geo Consulting nor the
 *       names of its contributors may be used to endorse or promote
 *       products derived from this software without specific prior
 *       written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 ****************************************************************************/

#include "OutlierFilter.hpp"

#include <pdal/KDIndex.hpp>
#include <pdal/util/ProgramArgs.hpp>
#include <pdal/util/Utils.hpp>
#include <pdal/util/ThreadPool.hpp>

#include <numeric>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pdal
{

static StaticPluginInfo const s_info
{
    "filters.outlier",
    "Outlier removal",
    "http://pdal.io/stages/filters.outlier.html"
};

CREATE_STATIC_STAGE(OutlierFilter, s_info)

std::string OutlierFilter::getName() const
{
    return s_info.name;
}

void OutlierFilter::addArgs(ProgramArgs& args)
{
    args.add("method", "Method [default: statistical]", m_method, "statistical");
    args.add("min_k", "Minimum number of neighbors in radius", m_minK, 2);
    args.add("radius", "Radius", m_radius, 1.0);
    args.add("mean_k", "Mean number of neighbors", m_meanK, 8);
    args.add("multiplier", "Standard deviation threshold", m_multiplier, 2.0);
    args.add("class", "Class to use for noise points", m_class, ClassLabel::LowPoint);
    args.add("threads", "Number of threads used to run this filter", m_threads, 1U);
}

void OutlierFilter::addDimensions(PointLayoutPtr layout)
{
    layout->registerDim(Dimension::Id::Classification);
}

void OutlierFilter::ready(BasePointTable &table)
{
    if (m_threads < 1)
    {
        log()->get(LogLevel::Warning)
            << "Number of threads < 1 ("
            << m_threads << "). Setting to 1."
            << std::endl;
        m_threads = 1;
    }

    unsigned int hw_concurrency = std::thread::hardware_concurrency();
    if (m_threads > hw_concurrency)
    {
        log()->get(LogLevel::Warning)
            << "Number of threads (" << m_threads << ") "
            << "greater than available processors (" << hw_concurrency << "). "
            << "This can degrade performance."
            << std::endl;
    }
}

Indices OutlierFilter::processRadius(PointViewPtr inView)
{
    KD3Index index(*inView);
    index.build();

    point_count_t np = inView->size();

    PointIdList inliers, outliers;

    std::deque<pdal::PointId> point_indices(np);
    std::iota(point_indices.begin(), point_indices.end(), 0);

    std::mutex queue_lock, indices_lock;

    std::unique_ptr<ThreadPool> thread_pool(new ThreadPool(m_threads));

    for (unsigned int i = 0; i < m_threads; i++) {
        thread_pool->add([&] {
          for (;;) {
              pdal::PointId idx;
              {
                  // Get a point idx to process
                  std::lock_guard<std::mutex> lock(queue_lock);
                  if (point_indices.empty())
                      return;
                  idx = point_indices.front();
                  point_indices.pop_front();
              }
              // Do expensive work.
              auto ids = index.radius(idx, m_radius);
              {
                  // Put point into appropriate container.
                  std::lock_guard<std::mutex> lock(indices_lock);
                  if (ids.size() > size_t(m_minK))
                      inliers.push_back(idx);
                  else
                      outliers.push_back(idx);
              }
          }
        });
    }
    thread_pool->go();
    thread_pool->join();

    return Indices{inliers, outliers};
}

Indices OutlierFilter::processStatistical(PointViewPtr inView)
{
    KD3Index index(*inView);
    index.build();

    point_count_t np = inView->size();

    PointIdList inliers, outliers;

    std::deque<pdal::PointId> point_indices(np);
    std::iota(point_indices.begin(), point_indices.end(), 0);

    std::vector<double> distances(np, 0.0);

    // we increase the count by one because the query point itself will
    // be included with a distance of 0
    point_count_t count = m_meanK + 1;

    std::mutex queue_lock, distances_lock;

    std::unique_ptr<ThreadPool> thread_pool(new ThreadPool(m_threads));

    for (unsigned int i = 0; i < m_threads; i++)
    {
        thread_pool->add([&] {
          for (;;) {
              pdal::PointId idx;
              {
                  // Get a point idx to process
                  std::lock_guard<std::mutex> lock(queue_lock);
                  if (point_indices.empty())
                      return;

                  idx = point_indices.front();
                  point_indices.pop_front();
              }

              // Do expensive work.
              PointIdList indices(count);
              std::vector<double> sqr_dists(count);
              index.knnSearch(idx, count, &indices, &sqr_dists);
              double tmp_distance = distances[idx];
              for (size_t j = 1; j < count; ++j) {
                  double delta = std::sqrt(sqr_dists[j]) - tmp_distance;
                  tmp_distance += (delta / j);
              }

              {
                  // Lock distances vector and update it
                  std::lock_guard<std::mutex> lock(distances_lock);
                  distances[idx] = tmp_distance;
              }
          }
        });
    }
    thread_pool->go();
    thread_pool->join();

    size_t n(0);
    double M1(0.0);
    double M2(0.0);
    for (auto const& d : distances)
    {
        size_t n1(n);
        n++;
        double delta = d - M1;
        double delta_n = delta / n;
        M1 += delta_n;
        M2 += delta * delta_n * n1;
    }
    double mean = M1;
    double variance = M2 / (n - 1.0);
    double stdev = std::sqrt(variance);

    double threshold = mean + m_multiplier * stdev;

    for (PointId i = 0; i < np; ++i)
    {
        if (distances[i] < threshold)
            inliers.push_back(i);
        else
            outliers.push_back(i);
    }

    return Indices{inliers, outliers};
}

PointViewSet OutlierFilter::run(PointViewPtr inView)
{
    PointViewSet viewSet;
    if (!inView->size())
        return viewSet;

    Indices indices;
    if (Utils::iequals(m_method, "statistical"))
    {
        indices = processStatistical(inView);
    }
    else if (Utils::iequals(m_method, "radius"))
    {
        indices = processRadius(inView);
    }
    else
    {
        log()->get(LogLevel::Warning) << "Requested method is unrecognized. "
                                         "Please choose from \"statistical\" "
                                         "or \"radius\".\n";
        viewSet.insert(inView);
        return viewSet;
    }

    if (indices.inliers.empty())
    {
        log()->get(LogLevel::Warning) << "Requested filter would remove all "
                                         "points. Try a larger radius/smaller "
                                         "minimum neighbors.\n";
        viewSet.insert(inView);
        return viewSet;
    }

    if (!indices.outliers.empty())
    {
        log()->get(LogLevel::Debug2)
            << "Labeled " << indices.outliers.size() << " outliers as noise!\n";

        // set the classification label of outlier returns
        for (const auto& i : indices.outliers)
            inView->setField(Dimension::Id::Classification, i, m_class);

        viewSet.insert(inView);
    }
    else
    {
        if (indices.outliers.empty())
            log()->get(LogLevel::Warning)
                << "Filtered cloud has no outliers!\n";

        // return the input buffer unchanged
        viewSet.insert(inView);
    }

    return viewSet;
}

} // namespace pdal
