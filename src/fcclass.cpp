// FIXME Check for memory leaks and corruptions in numpy-like return types
// FIXME Check if RowMajor order is the right thing to do everywhere
// FIXME I still don't like the way weights/biasses are handled
// TODO Regularization
// TODO Batch gradient computation
// FIXME Unify interface for x_input; right now its either an Eigen matrix or
//       a list of vectors
// FIXME There are many places that could benefit from using a zip iterator
// FIXME Needs a better random initialization

#include <pybind11/eigen.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <algorithm>
#include <iostream>
#include <random>

#include "fcclass.hpp"
// #include "cost.hpp"

namespace py = pybind11;
using namespace pybind11::literals;

FcClassifier::FcClassifier(size_t input_units, const shape_t hidden_units)
    : layers(hidden_units.size() + 1), costfun(cross_entropy) {
  const auto n_layers = hidden_units.size() + 1;
  if (n_layers < 1) {
    throw std::invalid_argument("Number of layers is too small");
  }

  // Create temporary shapes array to unify loop below
  size_t shapes[n_layers + 1];
  shapes[0] = input_units;
  shapes[n_layers] = 1;
  copy(hidden_units.begin(), hidden_units.end(), shapes + 1);

  // Initialize the weights with zeros
  for (size_t n = 0; n < n_layers; ++n) {
    layers[n].weights = ematrix_t::Zero(shapes[n + 1], shapes[n]);
    layers[n].biases = evector_t::Zero(shapes[n + 1]);
    layers[n].activation = sigmoid;
  }
}

shape_t FcClassifier::hidden_units() const {
  shape_t result(layers.size() - 1);
  for (size_t i = 0; i < layers.size() - 1; ++i) {
    result[i] = layers[i].biases.size();
  }
  return result;
}

void FcClassifier::init_random(long seed) {
  srand(seed);
  for (auto &layer : layers) {
    layer.weights =
        ematrix_t::Random(layer.weights.rows(), layer.weights.cols());
    layer.biases = evector_t::Random(layer.biases.size());
  }
}

std::vector<weights_biases_t> FcClassifier::get_weights() const {
  std::vector<weights_biases_t> result(layers.size());
  for (size_t i = 0; i < layers.size(); ++i) {
    result[i] = weights_biases_t(layers[i].weights, layers[i].biases);
  }
  return result;
}

void FcClassifier::set_weights(const size_t layer,
                               const ecref<ematrix_t> weights,
                               const ecref<evector_t> biases) {
  if ((weights.rows() != layers[layer].weights.rows()) ||
      (weights.cols() != layers[layer].weights.cols())) {
    // FIXME Better error message
    throw std::invalid_argument("Set weights have wrong shape");
  }
  if (biases.size() != layers[layer].biases.size()) {
    throw std::invalid_argument("Set biases have wrong shape");
  }

  layers[layer].weights = weights;
  layers[layer].biases = biases;
}

// Note that x_in in TensorFlow like with the sample index being the last
// one
evector_t FcClassifier::predict(const ecref<ematrix_t> x_in) const {
  ematrix_t activation = x_in;
  for (auto const &layer : layers) {
    auto lin_activation = (layer.weights * activation).colwise() + layer.biases;
    activation = lin_activation.unaryExpr(layer.activation.f);
  }

  return activation.row(0);
}

double FcClassifier::evaluate(const ecref<ematrix_t> x_in,
                              const ecref<evector_t> y_in) const {
  if (x_in.cols() != y_in.size()) {
    std::stringstream errmsg;
    errmsg << "Number of samples does not match " << x_in.cols()
           << " != " << y_in.size() << std::endl;
    throw std::invalid_argument(errmsg.str());
  }

  auto y_hat = predict(x_in);
  auto result = 0.0;
  for (auto i = 0; i < y_hat.size(); ++i) {
    result += costfun.f(y_in[i], y_hat[i]);
  }
  return result;
}

std::pair<double, std::vector<weights_biases_t>> FcClassifier::back_propagate(
    const ecref<evector_t> x_input, const double y_input) const {
  // Foward propagate to compute activations
  evector_t activations[layers.size()];
  evector_t lin_activations[layers.size()];

  lin_activations[0] = layers[0].weights * x_input + layers[0].biases;
  activations[0] = lin_activations[0].unaryExpr(layers[0].activation.f);

  for (size_t i = 1; i < layers.size(); ++i) {
    lin_activations[i] =
        layers[i].weights * activations[i - 1] + layers[i].biases;
    activations[i] = lin_activations[i].unaryExpr(layers[i].activation.f);
  }

  // Back propagate to compute gradients
  std::vector<weights_biases_t> gradients(layers.size());
  evector_t buf(1);
  buf[0] = costfun.d2f(y_input, activations[layers.size() - 1][0]);

  for (size_t i = layers.size() - 1; i > 0; --i) {
    buf.array() *=
        lin_activations[i].unaryExpr(layers[i].activation.df).array();
    gradients[i] = weights_biases_t(buf * activations[i - 1].transpose(), buf);
    buf = layers[i].weights.transpose() * buf;
  }
  buf = buf.array() *
        lin_activations[0].unaryExpr(layers[0].activation.df).array();
  gradients[0] = weights_biases_t(buf * x_input.transpose(), buf);

  double cost = costfun.f(y_input, activations[layers.size() - 1][0]);
  const std::pair<double, std::vector<weights_biases_t>> result(cost,
                                                                gradients);
  return result;
}

double FcClassifier::train(const std::vector<ecref<evector_t>> x_input,
                           const std::vector<double> y_input,
                           const double learning_rate,
                           const unsigned int nr_epochs,
                           const unsigned int batch_size,
                           const unsigned int seed) {
  auto nr_samples = x_input.size();
  if (nr_samples != y_input.size()) {
    std::stringstream errmsg;
    errmsg << "Number of samples does not match " << nr_samples
           << " != " << y_input.size() << std::endl;
    throw std::invalid_argument(errmsg.str());
  }

  if (nr_epochs < 1) {
    // TODO Just evaluate nn and return cost
    throw std::invalid_argument("nr_epochs should be larger than 0");
  }

  std::vector<size_t> indices(nr_samples);
  std::iota(indices.begin(), indices.end(), 0);
  auto rgen = std::default_random_engine(seed);
  auto cost_train = 0.0;

  for (size_t epoch = 0; epoch < nr_epochs; ++epoch) {
    std::shuffle(indices.begin(), indices.end(), rgen);
    cost_train =
        train_epoch(x_input, y_input, indices, learning_rate, batch_size);
  }

  return cost_train;
}

double FcClassifier::train_epoch(const std::vector<ecref<evector_t>> x_input,
                                 const std::vector<double> y_input,
                                 const std::vector<size_t> indices,
                                 const double learning_rate,
                                 const unsigned int batch_size) {
  auto cost = 0.0;
  size_t batch_counter = 0;
  const size_t nr_layers = layers.size();

  // Initilaize the temporary gradients
  weights_biases_t gradients[nr_layers];
  for (size_t i = 0; i < nr_layers; ++i) {
    gradients[i].first =
        ematrix_t::Zero(layers[i].weights.rows(), layers[i].weights.cols());
    gradients[i].second = evector_t::Zero(layers[i].biases.size());
  }

  for (size_t n = 0; n < indices.size(); ++n) {
    const auto index = indices[n];
    auto result = back_propagate(x_input[index], y_input[index]);
    cost += result.first;

    for (size_t i = 0; i < nr_layers; ++i) {
      gradients[i].first += result.second[i].first;
      gradients[i].second += result.second[i].second;
    }
    batch_counter++;

    if ((batch_counter >= batch_size) or (n >= indices.size() - 1)) {
      for (size_t i = 0; i < nr_layers; ++i) {
        // Update weights
        const auto alpha = learning_rate / batch_counter;
        layers[i].weights -= alpha * gradients[i].first;
        layers[i].biases -= alpha * gradients[i].second;

        // Reset temporary variables
        gradients[i].first.setZero();
        gradients[i].second.setZero();
      }
      batch_counter = 0;
    }
  }

  return cost;
}
