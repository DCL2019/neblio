#include "ntp1sendtxdata.h"

#include "ntp1sendtxdata.h"

#include "init.h"
#include "util.h"
#include "wallet.h"
#include "json/json_spirit.h"

const std::string NTP1SendTxData::NEBL_TOKEN_ID = "NEBL";

std::vector<NTP1OutPoint> NTP1SendTxData::getUsedInputs() const
{
    if (!ready)
        throw std::runtime_error("NTP1SendTxData not ready; cannot get used inputs");
    return tokenSourceInputs;
}

std::map<std::string, int64_t> NTP1SendTxData::getChangeTokens() const
{
    if (!ready)
        throw std::runtime_error("NTP1SendTxData not ready; cannot get change amounts");
    return totalChangeTokens;
}

NTP1SendTxData::NTP1SendTxData() { /*fee = 0;*/}

std::map<std::string, int64_t>
CalculateRequiredTokenAmounts(const std::vector<NTP1SendTokensOneRecipientData>& recipients)
{
    std::map<std::string, int64_t> required_amounts;
    for (const auto& r : recipients) {
        if (required_amounts.find(r.tokenId) == required_amounts.end()) {
            required_amounts[r.tokenId] = 0;
        }
        required_amounts[r.tokenId] += r.amount;
    }
    return required_amounts;
}

// get available balances, either from inputs (if provided) or from the wallet
std::map<string, int64_t> GetAvailableTokenBalances(boost::shared_ptr<NTP1Wallet>    wallet,
                                                    const std::vector<NTP1OutPoint>& inputs,
                                                    bool useBalancesFromWallet)
{
    std::map<string, int64_t> balancesMap;
    if (useBalancesFromWallet) {
        // get token balances from the wallet
        balancesMap = wallet->getBalancesMap();
    } else {
        // loop over all inputs and collect the total amount of tokens available
        for (const auto& input : inputs) {
            std::unordered_map<NTP1OutPoint, NTP1Transaction> availableOutputsMap =
                wallet->getWalletOutputsWithTokens();
            auto it = availableOutputsMap.find(input);
            if (it != availableOutputsMap.end()) {
                const NTP1Transaction& ntp1tx = it->second;
                if (input.getIndex() + 1 > ntp1tx.getTxOutCount()) {
                    throw std::runtime_error("An output you have of transaction " +
                                             ntp1tx.getTxHash().ToString() +
                                             " claims that you have an invalid output number: " +
                                             ::ToString(input.getIndex()));
                }
                // loop over tokens
                for (int i = 0; i < (int)ntp1tx.getTxOut(input.getIndex()).getNumOfTokens(); i++) {
                    const NTP1TokenTxData& tokenT    = ntp1tx.getTxOut(input.getIndex()).getToken(i);
                    auto                   balanceIt = balancesMap.find(tokenT.getTokenId());
                    if (balanceIt == balancesMap.end()) {
                        balancesMap[tokenT.getTokenId()] = 0;
                    }
                    balancesMap[tokenT.getTokenId()] += tokenT.getAmount();
                }
            }
        }
    }
    return balancesMap;
}

int64_t CalculateTotalNeblsInInputs(std::vector<NTP1OutPoint> inputs)
{
    {
        std::unordered_set<NTP1OutPoint> inputsSet(inputs.begin(), inputs.end());
        inputs = std::vector<NTP1OutPoint>(inputsSet.begin(), inputsSet.end());
    }

    int64_t currentTotalNeblsInSelectedInputs = 0;
    for (const auto& input : inputs) {
        auto it = pwalletMain->mapWallet.find(input.getHash());
        if (it == pwalletMain->mapWallet.end()) {
            throw std::runtime_error("The transaction: " + input.getHash().ToString() +
                                     " was not found in the wallet.");
        }

        const CTransaction& tx = it->second;
        if (input.getIndex() + 1 > tx.vout.size()) {
            throw std::runtime_error("An invalid output index: " + ::ToString(input.getIndex()) +
                                     " of transaction " + input.getHash().ToString() + " was used.");
        }
        currentTotalNeblsInSelectedInputs += static_cast<int64_t>(tx.vout.at(input.getIndex()).nValue);
    }
    return currentTotalNeblsInSelectedInputs;
}

void NTP1SendTxData::selectNTP1Tokens(boost::shared_ptr<NTP1Wallet>                      wallet,
                                      const std::vector<COutPoint>&                      inputs,
                                      const std::vector<NTP1SendTokensOneRecipientData>& recipients,
                                      bool addMoreInputsIfRequired)
{
    std::vector<NTP1OutPoint> ntp1OutPoints;
    std::transform(inputs.begin(), inputs.end(), std::back_inserter(ntp1OutPoints),
                   [](const COutPoint& o) { return NTP1OutPoint(o.hash, o.n); });

    selectNTP1Tokens(wallet, ntp1OutPoints, recipients, addMoreInputsIfRequired);
}

void NTP1SendTxData::selectNTP1Tokens(boost::shared_ptr<NTP1Wallet>               wallet,
                                      std::vector<NTP1OutPoint>                   inputs,
                                      std::vector<NTP1SendTokensOneRecipientData> recipients,
                                      bool addMoreInputsIfRequired)
{
    totalTokenAmountsInSelectedInputs.clear();
    tokenSourceInputs.clear();
    totalChangeTokens.clear();
    intermediaryTIs.clear();
    recipientsList.clear();
    usedWallet.reset();

    // remove non-NTP1 recipients (nebl recipients)
    recipients.erase(std::remove_if(recipients.begin(), recipients.end(),
                                    [](const NTP1SendTokensOneRecipientData& r) {
                                        return (r.tokenId == NTP1SendTxData::NEBL_TOKEN_ID);
                                    }),
                     recipients.end());

    // remove inputs duplicates
    {
        std::unordered_set<NTP1OutPoint> inputsSet(inputs.begin(), inputs.end());
        inputs = std::vector<NTP1OutPoint>(inputsSet.begin(), inputsSet.end());
    }

    // collect all required amounts in one map, with tokenId vs amount
    std::map<std::string, int64_t> targetAmounts = CalculateRequiredTokenAmounts(recipients);

    {
        uint64_t total = 0;
        for (const auto& ta : targetAmounts) {
            total += ta.second;
        }
        txHasNTP1Tokens = (total != 0);
    }

    // get available balances, either from inputs (if provided) or from the wallet
    std::map<string, int64_t> balancesMap =
        GetAvailableTokenBalances(wallet, inputs, addMoreInputsIfRequired);

    // check whether the required amounts can be covered by the available balances
    for (const auto& required_amount : targetAmounts) {
        if (required_amount.first == NTP1SendTxData::NEBL_TOKEN_ID) {
            // ignore nebls, deal only with tokens
            continue;
        }
        auto available_balance = balancesMap.find(required_amount.first);
        if (available_balance != balancesMap.end()) {
            if (required_amount.second > available_balance->second) {
                throw std::runtime_error("Your balance/selected inputs is not sufficient to cover for " +
                                         wallet->getTokenName(required_amount.first));
            }
        } else {
            throw std::runtime_error("You're trying to spend tokens that you don't own or are not "
                                     "included in the inputs you selected; namely: " +
                                     wallet->getTokenName(required_amount.first));
        }
    }

    // calculate reserved balances to be used in this transaction
    const std::unordered_map<NTP1OutPoint, NTP1Transaction> walletOutputsMap =
        wallet->getWalletOutputsWithTokens();
    std::deque<NTP1OutPoint> availableOutputs;
    if (addMoreInputsIfRequired) {
        // assume that inputs automatically has to be gathered from the wallet
        for (const auto& el : walletOutputsMap) {
            availableOutputs.push_back(el.first);
        }
        for (const auto& el : inputs) {
            tokenSourceInputs.push_back(el);
        }
    } else {
        for (const auto& el : inputs) {
            tokenSourceInputs.push_back(el);
            availableOutputs.push_back(el);
        }
    }

    // remove inputs duplicates
    {
        std::unordered_set<NTP1OutPoint> inputsSet(tokenSourceInputs.begin(), tokenSourceInputs.end());
        tokenSourceInputs = std::vector<NTP1OutPoint>(inputsSet.begin(), inputsSet.end());
    }

    // to improve privacy, shuffle inputs; pseudo-random is good enough here
    std::random_shuffle(availableOutputs.begin(), availableOutputs.end());

    // this container will be filled and must have tokens that are higher than the required amounts
    // reset fulfilled amounts and change to zero
    for (const std::pair<std::string, int64_t>& el : targetAmounts) {
        totalTokenAmountsInSelectedInputs[el.first] = 0;
        totalChangeTokens[el.first]                 = 0;
    }

    // fill tokenSourceInputs if inputs are not given
    for (const std::pair<std::string, int64_t>& targetAmount : targetAmounts) {
        for (int i = 0; i < (int)availableOutputs.size(); i++) {
            const auto& output   = availableOutputs.at(i);
            auto        ntp1TxIt = walletOutputsMap.find(output);
            if (ntp1TxIt == walletOutputsMap.end()) {
                // if the output is not found the NTP1 wallet outputs, it means that it doesn't have NTP1
                // tokens, so skip
                continue;
            }
            const NTP1Transaction& txData    = ntp1TxIt->second;
            const NTP1TxOut&       ntp1txOut = txData.getTxOut(output.getIndex());

            auto numOfTokensInOutput = ntp1txOut.getNumOfTokens();
            bool takeThisOutput      = false;
            if (addMoreInputsIfRequired) {
                for (auto i = 0u; i < numOfTokensInOutput; i++) {
                    std::string outputTokenId = ntp1txOut.getToken(i).getTokenId();
                    // if token id matches in the transaction with the required one, take it into account
                    int64_t required_amount_still =
                        targetAmount.second - totalTokenAmountsInSelectedInputs[outputTokenId];
                    if (targetAmount.first == outputTokenId && required_amount_still > 0) {
                        takeThisOutput = true;
                        break;
                    }
                }
            } else {
                // take all prev outputs
                takeThisOutput = true;
            }

            // take this transaction by
            // 1. remove it from the vector of available outputs
            // 2. add its values to fulfilledTokenAmounts
            // 3. add the address to the list of inputs to use (pointless if a list of inputs was
            // provided)
            if (takeThisOutput) {
                for (auto i = 0u; i < numOfTokensInOutput; i++) {
                    std::string outputTokenId = ntp1txOut.getToken(i).getTokenId();
                    totalTokenAmountsInSelectedInputs[outputTokenId] +=
                        ntp1txOut.getToken(i).getAmount();
                }
                tokenSourceInputs.push_back(output);
                availableOutputs.erase(availableOutputs.begin() + i);
                i--;
                if (availableOutputs.size() == 0) {
                    break;
                }
            }
        }
    }

    // finally, verify that the balances caught cover the amount to be sent
    for (const std::pair<std::string, int64_t>& targetAmount : targetAmounts) {
        // we don't do NEBL calculations here
        if (targetAmount.first == NTP1SendTxData::NEBL_TOKEN_ID) {
            continue;
        }
        if (totalTokenAmountsInSelectedInputs[targetAmount.first] < targetAmount.second) {
            throw std::runtime_error("Failed to cover required balance of " +
                                     wallet->getTokenName(targetAmount.first));
        }
        totalChangeTokens[targetAmount.first] =
            totalTokenAmountsInSelectedInputs[targetAmount.first] - targetAmount.second;
    }

    recipientsList.assign(recipients.begin(), recipients.end());

    // remove empty elements from total from inputs
    for (auto it = totalTokenAmountsInSelectedInputs.begin();
         it != totalTokenAmountsInSelectedInputs.end();) {
        if (it->second == 0)
            it = totalTokenAmountsInSelectedInputs.erase(it);
        else
            ++it;
    }

    // remove empty elements from change
    for (auto it = totalChangeTokens.begin(); it != totalChangeTokens.end();) {
        if (it->second == 0)
            it = totalChangeTokens.erase(it);
        else
            ++it;
    }

    // remove inputs duplicates
    {
        std::unordered_set<NTP1OutPoint> inputsSet(tokenSourceInputs.begin(), tokenSourceInputs.end());
        tokenSourceInputs = std::vector<NTP1OutPoint>(inputsSet.begin(), inputsSet.end());
    }

    const std::unordered_map<NTP1OutPoint, NTP1Transaction> walletOutputs =
        wallet->getWalletOutputsWithTokens();

    // sort inputs by which has more tokens first
    std::sort(
        tokenSourceInputs.begin(), tokenSourceInputs.end(),
        [&walletOutputs](const NTP1OutPoint& o1, const NTP1OutPoint& o2) {
            auto it1    = walletOutputs.find(o1);
            auto it2    = walletOutputs.find(o2);
            int  count1 = 0;
            int  count2 = 0;
            if (it1 != walletOutputs.end()) {
                const NTP1Transaction& tx1 = it1->second;
                if (o1.getIndex() + 1 > tx1.getTxOutCount()) {
                    throw std::runtime_error(
                        "While sorting inputs in NTP1 selector, output index is out of range for: " +
                        o1.getHash().ToString() + ":" + ::ToString(o1.getIndex()));
                }
                count1 = tx1.getTxOut(o1.getIndex()).getNumOfTokens();
            }
            if (it2 != walletOutputs.end()) {
                const NTP1Transaction& tx2 = it2->second;
                if (o2.getIndex() + 1 > tx2.getTxOutCount()) {
                    throw std::runtime_error(
                        "While sorting inputs in NTP1 selector, output index is out of range for: " +
                        o2.getHash().ToString() + ":" + ::ToString(o2.getIndex()));
                }
                count2 = tx2.getTxOut(o2.getIndex()).getNumOfTokens();
            }
            return count1 > count2;
        });

    std::vector<NTP1SendTokensOneRecipientData> recps = recipients;
    for (const auto& in : tokenSourceInputs) {
        // get the output
        auto it = walletOutputs.find(in);

        if (it == walletOutputs.end()) {
            // No NTP1 token in this input
            continue;
        }
        // extract the transaction from the output
        const NTP1Transaction& ntp1tx = it->second;
        if (in.getIndex() + 1 > ntp1tx.getTxOutCount()) {
            throw std::runtime_error(
                "While attempting to credit recipients, input index is out of range for: " +
                in.getHash().ToString() + ":" + ::ToString(in.getIndex()));
        }

        IntermediaryTI iti;

        NTP1TxOut ntp1txOut = ntp1tx.getTxOut(in.getIndex());
        for (int i = 0; i < (int)ntp1txOut.getNumOfTokens(); i++) {
            NTP1TokenTxData& token = ntp1txOut.getToken(i);
            for (int j = 0; j < (int)recps.size(); j++) {
                // if the token id matches and the recipient needs more, give them that amount
                if (ntp1txOut.getToken(i).getTokenId() == recps[j].tokenId && recps[j].amount > 0) {
                    NTP1Script::TransferInstruction ti;

                    if (recps[j].amount >= token.getAmount()) {
                        recps[j].amount -= token.getAmount();
                        ti.amount = token.getAmount();
                        token.setAmount(0);
                    } else {
                        token.setAmount(token.getAmount() - recps[j].amount);
                        ti.amount       = recps[j].amount;
                        recps[j].amount = 0;
                    }

                    // add that this input will go to recipient j
                    ti.outputIndex = j;
                    ti.skipInput   = false;

                    iti.TIs.push_back(ti);
                }
            }
            iti.input = in;
            if (token.getAmount() > 0) {
                NTP1Script::TransferInstruction ti;
                ti.amount = token.getAmount();

                // add that this input will go to recipient j
                ti.outputIndex = IntermediaryTI::CHANGE_OUTPUT_FAKE_INDEX;
                ti.skipInput   = false;
                iti.TIs.push_back(ti);

                token.setAmount(0);
            }
        }

        intermediaryTIs.push_back(iti);
    }

    // make sure that all recipients have received their tokens
    for (const auto r : recps) {
        // we don't select nebls
        if (r.tokenId == NTP1SendTxData::NEBL_TOKEN_ID) {
            continue;
        }
        if (r.amount != 0) {
            throw std::runtime_error("The recipient " + r.destination + "; of token: " + r.tokenId +
                                     "; still has an unfulfilled amount: " + ::ToString(r.amount) +
                                     ". This should've been spotted earlier.");
        }
    }

    usedWallet = wallet;

    ready = true;
}

std::map<std::string, int64_t> NTP1SendTxData::getTotalTokensInInputs() const
{
    if (!ready)
        throw std::runtime_error("NTP1SendTxData not ready; cannot get total tokens in inputs");
    return totalTokenAmountsInSelectedInputs;
}

bool NTP1SendTxData::isReady() const { return ready; }

std::vector<NTP1SendTokensOneRecipientData> NTP1SendTxData::getNTP1TokenRecipientsList() const
{
    if (!ready)
        throw std::runtime_error("NTP1SendTxData not ready; cannot get the recipients list");
    return recipientsList;
}

boost::shared_ptr<NTP1Wallet> NTP1SendTxData::getWallet() const
{
    if (!ready)
        throw std::runtime_error("NTP1SendTxData not ready; cannot get the wallet used in calculations");
    return usedWallet;
}

std::vector<IntermediaryTI> NTP1SendTxData::getIntermediaryTIs() const { return intermediaryTIs; }

int64_t NTP1SendTxData::__addInputsThatCoversNeblAmount(uint64_t neblAmount)
{

    // get nebls that fulfill the fee (if required)

    uint64_t currentTotalNeblsInSelectedInputs = CalculateTotalNeblsInInputs(tokenSourceInputs);

    // check if the total amount in selected addresses is sufficient for the amount
    if (neblAmount > currentTotalNeblsInSelectedInputs) {
        std::vector<COutput> availableOutputs;
        pwalletMain->AvailableCoins(availableOutputs);

        // shuffle outputs to select randomly
        std::random_shuffle(availableOutputs.begin(), availableOutputs.end());

        // add more outputs
        for (const auto& output : availableOutputs) {
            NTP1OutPoint outPoint(output.tx->GetHash(), output.i);
            // skip if already in
            if (std::find(tokenSourceInputs.begin(), tokenSourceInputs.end(), outPoint) !=
                tokenSourceInputs.end()) {
                continue;
            }
            tokenSourceInputs.push_back(outPoint);
            currentTotalNeblsInSelectedInputs = CalculateTotalNeblsInInputs(tokenSourceInputs);
            if (currentTotalNeblsInSelectedInputs >= neblAmount) {
                break;
            }
        }
    }

    return currentTotalNeblsInSelectedInputs;
}

bool NTP1SendTxData::hasNTP1Tokens() const { return txHasNTP1Tokens; }

uint64_t NTP1SendTxData::getRequiredNeblsForOutputs() const
{
    if (!ready)
        throw std::runtime_error("NTP1SendTxData not ready; cannot get required fees");
    if (intermediaryTIs.size() > 0) {
        return MIN_TX_FEE * (recipientsList.size() + 1); // + 1 is for OP_RETURN output
    } else {
        return 0;
    }
}

int64_t NTP1SendTxData::EstimateTxSizeInBytes(int64_t num_of_inputs, int64_t num_of_outputs)
{
    return num_of_inputs * 181 + num_of_outputs * 34 + 10;
}

int64_t NTP1SendTxData::EstimateTxFee(int64_t num_of_inputs, int64_t num_of_outputs)
{
    double Fee = static_cast<double>(MIN_TX_FEE) *
                 (static_cast<double>(EstimateTxSizeInBytes(num_of_inputs, num_of_outputs)) / 1000.);
    // nearest 10000
    return static_cast<int64_t>(std::ceil(Fee / 10000) * 10000);
}

void NTP1SendTxData::FixTIsChangeOutputIndex(std::vector<NTP1Script::TransferInstruction>& TIs,
                                             int changeOutputIndex)
{
    for (auto& ti : TIs) {
        if (ti.outputIndex == IntermediaryTI::CHANGE_OUTPUT_FAKE_INDEX) {
            ti.outputIndex = changeOutputIndex;
        }
    }
}